/*

MIT License

Copyright (c) 2024 Oliver Schmidt (https://a2retro.de/)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#include <string.h>
#include <stdio.h>
#include <pico/stdlib.h>

#include "board.h"
#include "config.h"
#include "hdd.h"
#include "diskio.h"

#include "sp.h"

#define PRODOS_CMD_STATUS   0x00
#define PRODOS_CMD_READ     0x01
#define PRODOS_CMD_WRITE    0x02

#define PRODOS_I_CMD    0
#define PRODOS_I_UNIT   1
#define PRODOS_I_BLOCK  2
#define PRODOS_I_BUFFER 4

#define PRODOS_O_RETVAL 0
#define PRODOS_O_BUFFER 1

#define SP_CMD_STATUS   0x00
#define SP_CMD_READBLK  0x01
#define SP_CMD_WRITEBLK 0x02
#define SP_CMD_FORMAT   0x03
#define SP_CMD_CONTROL  0x04
#define SP_CMD_INIT     0x05
#define SP_CMD_OPEN     0x06
#define SP_CMD_CLOSE    0x07
#define SP_CMD_READ     0x08
#define SP_CMD_WRITE    0x09

#define SP_I_CMD    0
#define SP_I_PARAMS 2
#define SP_I_BUFFER 10

#define SP_O_RETVAL 0
#define SP_O_BUFFER 1

#define SP_PARAM_UNIT   0
#define SP_PARAM_CODE   3
#define SP_PARAM_BLOCK  3

#define SP_STATUS_STS   0x00
#define SP_STATUS_DCB   0x01
#define SP_STATUS_NLS   0x02
#define SP_STATUS_DIB   0x03

#define SP_SUCCESS  0x00
#define SP_BADCMD   0x01
#define SP_BUSERR   0x06
#define SP_BADCTL   0x21

volatile uint8_t  sp_control;
volatile uint8_t  sp_buffer[1024];
volatile uint16_t sp_read_offset;
volatile uint16_t sp_write_offset;

static uint8_t unit_to_drive(uint8_t unit) {
    uint8_t drive = unit >> 7;
    if ((unit >> 4 & 0x07) != board_slot()) {
        drive += 0x02;
    } 
    return drive;
}

void sp_init(void) {
    disk_init();            //  Settup the cache

    hdd_init();

#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif
}

//  Compiled code for read transfers
uint16_t sp_buffer_addr = 0;
uint16_t pd_buffer_addr = 0;

//  Instructions
#define INST_LDY        0xA0    //  + 1 byte imm
#define INST_STY        0x8C    //  + 2 byte addr
#define INST_INY        0xC8    //  
#define INST_RTS        0x60    //
#define INST_NOP        0xEA    
#define INST_JMP        0x4C    //  + 2 byte addr
#define INST_JMP_SIZE   3       //  1 + 2 byte addr

#define INST_BASE       0xCB00
#define INST_BASE_LO    0x00
#define INST_BASE_HI    0xCB

#define INST_PAGE_BITS  8
#define INST_PAGE_SIZE  (1L << INST_PAGE_BITS)


int __time_critical_func(check_buffer_wrap)(int instruction_index, int next_instruction_size) {
    int current_page = instruction_index >> INST_PAGE_BITS;                     //  Divide by INST_PAGE_SIZE (256)
    int remaining = ((current_page + 1) << INST_PAGE_BITS) - (instruction_index + next_instruction_size + INST_JMP_SIZE);

    if (remaining >= 0) {
        return instruction_index;
    } else {
        int nop_index_end = (((current_page + 1) << INST_PAGE_BITS) - INST_JMP_SIZE);

        //  fill in the last of this page
        while (instruction_index < nop_index_end) {
            firmware_code_buffer[instruction_index++] = INST_NOP;
        }

        // The end of the buffer needs to jump to the begining to trigger a page switch
        firmware_code_buffer[instruction_index++] = INST_JMP;
        firmware_code_buffer[instruction_index++] = INST_BASE_LO;
        firmware_code_buffer[instruction_index++] = INST_BASE_HI;
    }

    return instruction_index;
}


void __time_critical_func(sp_compile_buffer)(uint16_t a2_buffer_addr, uint8_t *in_buffer) {
    int i = 0;
    int current_page = 0;

    //  Reset the firmware pointer
    firmware_map[SP_CODE_MAP1] = firmware_code_buffer;
    firmware_map[SP_CODE_MAP2] = firmware_code_buffer;

    uint8_t last_value = 0;

    for (int buffer_index = 0; buffer_index < 512; buffer_index++) {
        uint8_t addr_lo = a2_buffer_addr & 0xFF;
        uint8_t addr_hi = (a2_buffer_addr >> 8) & 0xFF;

        uint8_t value = in_buffer[buffer_index];
        if ((last_value != value) || (buffer_index == 0)) {
            //  Emit a LDY + STY

            //  Add a JMP if needed, do a quick check to see if we are close
            if ((i % INST_PAGE_SIZE) >= (INST_PAGE_SIZE - (5 + INST_JMP_SIZE)))      //  5 is next inst size
                i = check_buffer_wrap(i, 5);

            firmware_code_buffer[i++] = INST_LDY;
            firmware_code_buffer[i++] = in_buffer[buffer_index];

            firmware_code_buffer[i++] = INST_STY;
            firmware_code_buffer[i++] = addr_lo;
            firmware_code_buffer[i++] = addr_hi;
        } else {
            //  Add a JMP if needed, do a quick check to see if we are close
            if ((i % INST_PAGE_SIZE) >= (INST_PAGE_SIZE - (3 + INST_JMP_SIZE)))      //  3 is next inst size
                i = check_buffer_wrap(i, 3);

            //  Emit a STY
            firmware_code_buffer[i++] = INST_STY;
            firmware_code_buffer[i++] = addr_lo;
            firmware_code_buffer[i++] = addr_hi;
        }

        last_value = value;

        a2_buffer_addr++;
    }

    //  Terminate with an RTS
    i = check_buffer_wrap(i, 1);
    firmware_code_buffer[i++] = INST_RTS;
}

void __time_critical_func(sp_reset)(void) {
    sp_control = CONTROL_NONE;
    sp_read_offset = sp_write_offset = 0;
    sp_buffer[0] = sp_buffer[1] = 0;
}

static uint8_t sp_stat(uint8_t *params, uint8_t *stat_list) {
    if (!params[SP_PARAM_UNIT]) {
        if (params[SP_PARAM_CODE] == SP_STATUS_STS) {
            printf("SP CmdStatus(Device=Smartport)\n");
            stat_list[2 + 0] = config_drives();
            stat_list[2 + 1] = 0b01000000;  // no interrupt sent
            memset(&stat_list[2 + 2], 0x00, 6);
            stat_list[0] = 8;   // size header low
            stat_list[1] = 0;   // size header high
        } else {
            return SP_BADCTL;
        }
    } else {
        if (params[SP_PARAM_CODE] == SP_STATUS_STS ||
            params[SP_PARAM_CODE] == SP_STATUS_DIB) {
            bool status = params[SP_PARAM_CODE] == SP_STATUS_STS;
            if (!hdd_status(params[SP_PARAM_UNIT] - 1, &stat_list[2 + 1])) {
                stat_list[2 + 0] = hdd_protected(params[SP_PARAM_UNIT] - 1)
                                 ? 0b11110100   // block, write, read, online, protected
                                 : 0b11110000;  // block, write, read, online
            } else {
                stat_list[2 + 0] = 0b11100000;  // block, write, read
                stat_list[2 + 1] = 0x00;        // blocks low
                stat_list[2 + 2] = 0x00;        // blocks mid
            }
            stat_list[2 + 3] = 0x00;    // blocks high
            if (status) {
                stat_list[0] = 4;   // size header low
                stat_list[1] = 0;   // size header high                    
            } else {
                stat_list[2 +  4] = 0x0A;   // id string length
                memcpy(&stat_list[2 + 5], "A2RETRONET      ", 16);
                stat_list[2 + 21] = 0x02;   // hard disk
                stat_list[2 + 22] = 0x00;   // removable
                stat_list[2 + 23] = 0x01;   // firmware version low
                stat_list[2 + 24] = 0x00;   // firmware version high
                stat_list[0] = 25;  // size header low
                stat_list[1] = 0;   // size header high    
            }
        } else {
            return SP_BADCTL;
        }
    }
    return SP_SUCCESS;
}

static uint8_t sp_readblk(uint8_t *params, uint8_t *buffer) {
    return hdd_read(params[SP_PARAM_UNIT] - 1, *(uint16_t*)&params[SP_PARAM_BLOCK], buffer);
}

static uint8_t sp_writeblk(uint8_t *params, const uint8_t *buffer) {
    return hdd_write(params[SP_PARAM_UNIT] - 1, *(uint16_t*)&params[SP_PARAM_BLOCK], buffer);
}

void sp_task(void) {

    if (sp_control == CONTROL_NONE || sp_control == CONTROL_DONE) {
        disk_task();
        return;
    }

    if (!hdd_sd_mounted() && !hdd_usb_mounted()) {
        return;
    }

    if (sp_control == CONTROL_CONFIG) {
        config();
        return;
    }
       
#ifdef PICO_DEFAULT_LED_PIN
    gpio_put(PICO_DEFAULT_LED_PIN, true);
#endif

//    printf("SP Cmd(Type=$%02X,Bytes=$%04X)\n", sp_control, sp_write_offset);
    switch (sp_control) {

        case CONTROL_PRODOS:
            switch (sp_buffer[PRODOS_I_CMD]) {
                case PRODOS_CMD_STATUS:
                    sp_buffer[PRODOS_O_RETVAL] = hdd_status(unit_to_drive(sp_buffer[PRODOS_I_UNIT]),
                                                            (uint8_t*)&sp_buffer[PRODOS_O_BUFFER]);
                    break;
                case PRODOS_CMD_READ:
                    uint16_t a2_buffer_address = (uint16_t)(((uint16_t)sp_address_high << 8) | sp_address_low);     //  SMARTPORT.S fills this in
                    pd_buffer_addr = a2_buffer_address;

                    sp_buffer[PRODOS_O_RETVAL] = hdd_read(unit_to_drive(sp_buffer[PRODOS_I_UNIT]),
                                                          *(uint16_t*)&sp_buffer[PRODOS_I_BLOCK],
                                                          (uint8_t*)&sp_buffer[PRODOS_O_BUFFER]);
#if FEATURE_A2F_PDMA
                    sp_compile_buffer(a2_buffer_address, (uint8_t*)&sp_buffer[PRODOS_O_BUFFER]);
#endif
                    //  Reset the address
                    sp_address_low = 0;
                    sp_address_high = 0;
                    break;
                case PRODOS_CMD_WRITE:
                    sp_buffer[PRODOS_O_RETVAL] = hdd_write(unit_to_drive(sp_buffer[PRODOS_I_UNIT]),
                                                           *(uint16_t*)&sp_buffer[PRODOS_I_BLOCK],
                                                           (uint8_t*)&sp_buffer[PRODOS_I_BUFFER]);
                    break;
                default:
                    printf("SP NO PD COMMAND\n");
                    break;
            }
            break;

        case CONTROL_SP:
//            printf("SP Cmd(Type=$%02X,SP_I_CMD=$%02X)\n", sp_control, sp_buffer[SP_I_CMD]);

            switch (sp_buffer[SP_I_CMD]) {
                case SP_CMD_STATUS:
//                    printf("SP CmdStatus(Device=$%02X)\n", sp_buffer[SP_I_PARAMS]);
                    sp_buffer[SP_O_RETVAL] = sp_stat((uint8_t*)&sp_buffer[SP_I_PARAMS],
                                                     (uint8_t*)&sp_buffer[SP_O_BUFFER]);
                    break;
                case SP_CMD_READBLK:
//                    printf("SP CmdReadBlock(Device=$%02X)\n", sp_buffer[SP_I_PARAMS]);
                    uint16_t a2_buffer_address = (uint16_t)(((uint16_t)sp_address_high << 8) | sp_address_low);     //  SMARTPORT.S fills this in
                    pd_buffer_addr = a2_buffer_address;

                    sp_buffer[SP_O_RETVAL] = hdd_read(sp_buffer[SP_I_PARAMS + SP_PARAM_UNIT] - 1, 
                                                        *(uint16_t*)&sp_buffer[SP_I_PARAMS + SP_PARAM_BLOCK], 
                                                        (uint8_t*)&sp_buffer[SP_O_BUFFER]);

#if FEATURE_A2F_PDMA
                    sp_compile_buffer(a2_buffer_address, (uint8_t*)&sp_buffer[SP_O_BUFFER]);
#endif
                    //  Reset the address
                    sp_address_low = 0;
                    sp_address_high = 0;
                    break;
                case SP_CMD_WRITEBLK:
//                    printf("SP CmdWriteBlock(Device=$%02X)\n", sp_buffer[SP_I_PARAMS]);

                    sp_buffer[SP_O_RETVAL] = hdd_write(sp_buffer[SP_I_PARAMS + SP_PARAM_UNIT] - 1, 
                                                        *(uint16_t*)&sp_buffer[SP_I_PARAMS + SP_PARAM_BLOCK], 
                                                        (uint8_t*)&sp_buffer[SP_I_BUFFER]);
                    break;
                case SP_CMD_FORMAT:
                    printf("SP CmdFormat(Device=$%02X)\n", sp_buffer[SP_I_PARAMS]);
                    sp_buffer[SP_O_RETVAL] = SP_BADCMD;
                    break;
                case SP_CMD_CONTROL:
                    printf("SP CmdControl(Device=$%02X)\n", sp_buffer[SP_I_PARAMS]);
                    sp_buffer[SP_O_RETVAL] = SP_BADCMD;
                    break;
                case SP_CMD_INIT:
                    printf("SP CmdInit(Device=$%02X)\n", sp_buffer[SP_I_PARAMS]);
                    sp_buffer[SP_O_RETVAL] = SP_SUCCESS;
                    break;
                case SP_CMD_OPEN:
                    printf("SP CmdOpen(Device=$%02X)\n", sp_buffer[SP_I_PARAMS]);
                    sp_buffer[SP_O_RETVAL] = SP_BADCMD;
                    break;
                case SP_CMD_CLOSE:
                    printf("SP CmdClose(Device=$%02X)\n", sp_buffer[SP_I_PARAMS]);
                    sp_buffer[SP_O_RETVAL] = SP_BADCMD;
                    break;
                case SP_CMD_READ:
                    printf("SP CmdRead(Device=$%02X)\n", sp_buffer[SP_I_PARAMS]);
                    sp_buffer[SP_O_RETVAL] = SP_BADCMD;
                    break;
                case SP_CMD_WRITE:
                    printf("SP CmdWrite(Device=$%02X)\n", sp_buffer[SP_I_PARAMS]);
                    sp_buffer[SP_O_RETVAL] = SP_BADCMD;
                    break;
                default:
                    printf("SP NO SP COMMAND\n");
                    break;
            }
            break;
    }

    sp_read_offset = sp_write_offset = 0;
    sp_control = CONTROL_DONE;

#ifdef PICO_DEFAULT_LED_PIN
    gpio_put(PICO_DEFAULT_LED_PIN, false);
#endif
}
