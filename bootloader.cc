// Bootloader.cc
// Copyright 2012 Olivier Gillet.
//
// Author: Olivier Gillet (ol.gillet@gmail.com)
// Modified for SMR project: Dan Green (danngreen1@gmail.com) 2015

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.

#include "stm32f4xx.h"

#include "system.h"

#include <cstring>

#include "../stmlib/dsp/dsp.h"
#include "../stmlib/utils/ring_buffer.h"
#include "../stmlib/system/bootloader_utils.h"
#include "../stmlib/system/flash_programming.h"
#include "../stmlib/system/system_clock.h"

//#include "../stm-audio-bootloader/qpsk/packet_decoder.h"
//#include "../stm-audio-bootloader/qpsk/demodulator.h"
#include "../stm-audio-bootloader/fsk/packet_decoder.h"
#include "../stm-audio-bootloader/fsk/demodulator.h"

extern "C" {
#include <stddef.h> /* size_t */
#include "inouts.h"
#include "codec.h"
#include "i2s.h"
#include "pca9685_driver.h"

#define delay(x)						\
do {							\
  register unsigned int i;				\
  for (i = 0; i < x; ++i)				\
    __asm__ __volatile__ ("nop\n\t":::"memory");	\
} while (0)

}

const int LED_LOCK[6]={LED_LOCK1, LED_LOCK2, LED_LOCK3, LED_LOCK4, LED_LOCK5, LED_LOCK6};
const uint32_t slider_led[6]={LED_SLIDER1, LED_SLIDER2, LED_SLIDER3, LED_SLIDER4, LED_SLIDER5, LED_SLIDER6};
#define ALL_SLIDERS (LED_SLIDER1|LED_SLIDER2|LED_SLIDER3|LED_SLIDER4|LED_SLIDER5|LED_SLIDER6)
#define ALL_LOCK_LEDS (LED_LOCK1| LED_LOCK2| LED_LOCK3| LED_LOCK4| LED_LOCK5| LED_LOCK6)

using namespace driver_system;
using namespace stmlib;
using namespace stm_audio_bootloader;


const float kSampleRate = 48000.0;
//const float kModulationRate = 6000.0; //QPSK 6000
//const float kBitRate = 12000.0; //QPSK 12000
uint32_t kStartExecutionAddress =		0x08008000;
uint32_t kStartReceiveAddress = 		0x08080000;
uint32_t EndOfMemory =					0x080FFFFC;

extern "C" {

void HardFault_Handler(void) { while (1); }
void MemManage_Handler(void) { while (1); }
void BusFault_Handler(void) { while (1); }
void UsageFault_Handler(void) { while (1); }
void NMI_Handler(void) { }
void SVC_Handler(void) { }
void DebugMon_Handler(void) { }
void PendSV_Handler(void) { }

}
System sys;
PacketDecoder decoder;
Demodulator demodulator;

uint16_t packet_index;
uint16_t old_packet_index=0;
uint8_t slider_i=0;

bool g_error;

enum UiState {
  UI_STATE_WAITING,
  UI_STATE_RECEIVING,
  UI_STATE_ERROR,
  UI_STATE_WRITING
};
volatile UiState ui_state;

extern "C" {

inline void *memcpy(void *dest, const void *src, size_t n)
{
    char *dp = (char *)dest;
    const char *sp = (const char *)src;
    while (n--)
        *dp++ = *sp++;
    return dest;
}


void update_slider_LEDs(void){
	static uint16_t dly=0;
	uint16_t fade_speed=800;

	if (ui_state == UI_STATE_RECEIVING){
		if (packet_index>old_packet_index){
			old_packet_index=packet_index;
			LED_SLIDER_OFF(ALL_SLIDERS);
			LED_SLIDER_ON(slider_led[slider_i]);
			if (++slider_i>=6) slider_i=0;

			//FSK only?

			if (((packet_index/6) % (78*2) )<78)
				LEDDriver_set_one_LED((packet_index/6) % 78, 500);
			else
				LEDDriver_set_one_LED((packet_index/6) % 78, 0);

		}
	} else if (ui_state == UI_STATE_WRITING){

		if (dly++>400){
			dly=0;
			LED_SLIDER_OFF(ALL_SLIDERS);
		} else if (dly==200){
			LED_SLIDER_ON(ALL_SLIDERS);
		}

	} else if (ui_state == UI_STATE_WAITING){

		if (dly==(fade_speed>>1)){ 		LEDDriver_set_one_LED(1, 500);LEDDriver_set_one_LED(2, 0);}
		if (dly++==fade_speed) {dly=0;	LEDDriver_set_one_LED(1, 0);LEDDriver_set_one_LED(2, 500);}


		//FSK only (needs to be optimized for QPSK)
		/*
		LEDDriver_set_one_LED(0, dly>fade_speed/2 ? dly : fade_speed-dly);
		LEDDriver_set_one_LED(1, dly>fade_speed/2 ? fade_speed-dly: dly);
		*/
	}

}

uint16_t State=0;
uint16_t manual_exit_primed;
bool exit_updater;

void check_button(void){
	uint16_t t;

	//Button depressed: ROTARY_SW=true, released: ROTARY_SW=false
	//Depressed adds a 0, released adds a 1

	if (ROTARY_SW) t=0xe000; else t=0xe001; //1110 0000 0000 000(0|1)
	State=(State<<1) | t;

	if (State == 0xff00)  	//Released event (depressed followed by released)
		manual_exit_primed=1;

	if (State == 0xe00f){ 				 //Depressed event (released followed by a bunch of depressed)
		if (packet_index==0 && manual_exit_primed==1)
			exit_updater=1;
	}

}

void SysTick_Handler() {
	system_clock.Tick();  // Tick global ms counter.
	update_slider_LEDs();
	check_button();
}

uint16_t discard_samples = 8000;

/*
void TIM4_IRQHandler(void)
{

	LED_ON(LED_LOCK[0]);

	if (!discard_samples) {
		bool sample = ROTUP ? true : false;

		if (sample) 	{LOCKJACK_ON;}
		else {LOCKJACK_OFF;}

		demodulator.PushSample(sample);
	} else {
		--discard_samples;
	}

	LED_OFF(LED_LOCK[0]);

	// Clear TIM4 update interrupt
	TIM_ClearITPendingBit(TIM4, TIM_IT_Update);


}
*/

void process_audio_block(int16_t *input, int16_t *output, uint16_t ht, uint16_t size){
	bool sample;
	static bool last_sample=false;
	int32_t t;

	LED_ON(LED_LOCK[5]);

	while (size) {
		size-=4;

		t=*input;

		if (last_sample==true){
			if (t < -300)
				sample=false;
			else
				sample=true;
		} else {
			if (t > 400)
				sample=true;
			else
				sample=false;
		}
		last_sample=sample;


		if (sample) LOCKJACK_ON;
		else LOCKJACK_OFF;

		if (!discard_samples) {
			demodulator.PushSample(sample);
		} else {
			--discard_samples;
		}

		if (ui_state == UI_STATE_ERROR)
			*output++=0;
		else
			*output++=*input;
		*output++=0;
		*output++=0;
		*output++=0;

		*input++;
		*input++;
		*input++;
		*input++;

	}
	LED_OFF(LED_LOCK[5]);

}


}

static uint32_t current_address;
static uint32_t kSectorBaseAddress[] = {
  0x08000000,
  0x08004000,
  0x08008000,
  0x0800C000,
  0x08010000,
  0x08020000,
  0x08040000,
  0x08060000,
  0x08080000,
  0x080A0000,
  0x080C0000,
  0x080E0000
};
const uint32_t kBlockSize = 16384;
const uint16_t kPacketsPerBlock = kBlockSize / kPacketSize;
uint8_t recv_buffer[kBlockSize];


inline void CopyMemory(uint32_t src_addr, uint32_t dst_addr, size_t size) {

	FLASH_Unlock();
	FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
				  FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR|FLASH_FLAG_PGSERR);


	for (size_t written = 0; written < size; written += 4) {

		//check if dst_addr is the start of a sector (in which case we should erase the sector)
		for (int32_t i = 0; i < 12; ++i) {
			if (dst_addr == kSectorBaseAddress[i]) {
				LED_ON(LED_LOCK[i % 6]);
				FLASH_EraseSector(i * 8, VoltageRange_3);
				LED_OFF(LED_LOCK[i % 6]);
			}
		}

		//Boundary check
		if (dst_addr > (kStartReceiveAddress-4)) //Do not overwrite receive buffer
			break;

		//Program the word
		FLASH_ProgramWord(dst_addr, *(uint32_t*)src_addr);

		src_addr += 4;
		dst_addr += 4;
	}

}


inline void ProgramPage(const uint8_t* data, size_t size) {
	LED_ON(LED_LOCK[4]);

	FLASH_Unlock();
	FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
				  FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR|FLASH_FLAG_PGSERR);
	for (int32_t i = 0; i < 12; ++i) {
		if (current_address == kSectorBaseAddress[i]) {
		  FLASH_EraseSector(i * 8, VoltageRange_3);
		}
	}
	const uint32_t* words = static_cast<const uint32_t*>(static_cast<const void*>(data));
	for (size_t written = 0; written < size; written += 4) {
		FLASH_ProgramWord(current_address, *words++);
		current_address += 4;
		if (current_address>=EndOfMemory){
			ui_state = UI_STATE_ERROR;
			g_error=true;
			break;
		}
	}

	LED_OFF(LED_LOCK[4]);
}

void init_audio_in(){

	//QPSK or Codec
	Codec_Init(48000);
	do {register unsigned int i; for (i = 0; i < 1000000; ++i) __asm__ __volatile__ ("nop\n\t":::"memory");} while (0);
	I2S_Block_Init();
	do {register unsigned int i; for (i = 0; i < 1000000; ++i) __asm__ __volatile__ ("nop\n\t":::"memory");} while (0);
	I2S_Block_PlayRec();

}

void Init() {
	sys.Init(false);
	system_clock.Init();
	init_inouts();
}

void LED_ring_startup(void){
	uint16_t i;
	uint32_t dly;
	uint8_t trail=8;

	LED_ON(LED_RING_OE); //actually turns the LED ring off
	LEDDriver_Init(5);
	for (i=0;i<26;i++)	LEDDriver_setRGBLED(i,0);
	LED_OFF(LED_RING_OE); //actually turns the LED ring on

	for (i=0;i<77+trail;i++){
		if (i<77) LEDDriver_set_one_LED(i, 500);
		delay(300000);
		if (i>=trail) LEDDriver_set_one_LED(i-trail, 0);
	}
}

void InitializeReception() {


	//FSK

	decoder.Init();
	decoder.Reset();

	demodulator.Init(16, 8, 4);
	demodulator.Sync();



	//QPSK
	/*
	decoder.Init(20000);
	demodulator.Init(
	 kModulationRate / kSampleRate * 4294967296.0,
	 kSampleRate / kModulationRate,
	 2.0 * kSampleRate / kBitRate);
	demodulator.SyncCarrier(true);
	decoder.Reset();
*/

	current_address = kStartReceiveAddress;
	packet_index = 0;
	old_packet_index = 0;
	slider_i = 0;
	ui_state = UI_STATE_WAITING;
}


int main(void) {
	uint32_t symbols_processed=0;
	uint32_t dly=0, button_debounce=0;
	uint8_t i;

//	InitializeReception(); //QPSK
	Init();
	InitializeReception(); //FSK

	dly=4000;
	while(dly--){
		if (ROTARY_SW) button_debounce++;
		else button_debounce=0;
	}
	exit_updater = (button_debounce>2000) ? 0 : 1;

	if (!exit_updater){
		LED_OFF(ALL_LOCK_LEDS);
		LED_SLIDER_OFF(ALL_SLIDERS);

		LED_ring_startup();

		init_audio_in(); //QPSK or Codec
		sys.StartTimers();
	}

	dly=4000;
	while(dly--){
		if (ROTARY_SW) button_debounce++;
		else button_debounce=0;
	}
	exit_updater = (button_debounce>2000) ? 0 : 1;

	manual_exit_primed=0;

	while (!exit_updater) {
		g_error = false;


		//QPSK
		/*
		if (demodulator.state() == DEMODULATOR_STATE_OVERFLOW){
			g_error = true;
			LED_ON(LED_LOCK[2]);
			LED_ON(LED_LOCK[3]);
		}else{
			demodulator.ProcessAtLeast(32);
		}
		 */


		while (demodulator.available() && !g_error && !exit_updater) {
			uint8_t symbol = demodulator.NextSymbol();
			PacketDecoderState state = decoder.ProcessSymbol(symbol);
			symbols_processed++;

			switch (state) {
				case PACKET_DECODER_STATE_OK:
				{
					ui_state = UI_STATE_RECEIVING;
					memcpy(recv_buffer + (packet_index % kPacketsPerBlock) * kPacketSize, decoder.packet_data(), kPacketSize);
					++packet_index;
					if ((packet_index % kPacketsPerBlock) == 0) {
						ui_state = UI_STATE_WRITING;
						ProgramPage(recv_buffer, kBlockSize);
						decoder.Reset();
						demodulator.Sync(); //FSK
						//demodulator.SyncCarrier(false);//QPSK
					} else {
						decoder.Reset(); //FSK
						//demodulator.SyncDecision();//QPSK
					}
				}
				break;

				case PACKET_DECODER_STATE_ERROR_SYNC:
					LED_ON(LED_LOCK[2]);
					g_error = true;
					break;

				case PACKET_DECODER_STATE_ERROR_CRC:
					LED_ON(LED_LOCK[3]);
					g_error = true;
					break;

				case PACKET_DECODER_STATE_END_OF_TRANSMISSION:
					exit_updater = true;
					LED_OFF(ALL_LOCK_LEDS);
					LED_ON(LED_LOCK[0]);
					LED_ON(LED_LOCK[5]);

					//Copy from Receive buffer to Execution memory

					CopyMemory(kStartReceiveAddress, kStartExecutionAddress, (current_address-kStartReceiveAddress));

					LED_ON(ALL_LOCK_LEDS);

					break;

				default:
					break;
			}
		}
		if (g_error) {
			ui_state = UI_STATE_ERROR;

			LED_ON(LED_LOCK[1]);
			while (!ROTARY_SW){;}

			LED_OFF(LED_LOCK[1]);
			while (ROTARY_SW){;}

			LED_OFF(ALL_LOCK_LEDS);
			LED_SLIDER_OFF(ALL_SLIDERS);

			for (i=0;i<26;i++){
				LEDDriver_setRGBLED(i,0);
			}

			InitializeReception();
			manual_exit_primed=0;
			exit_updater=false;
		}
	}

	Uninitialize();
	JumpTo(kStartExecutionAddress);

}
