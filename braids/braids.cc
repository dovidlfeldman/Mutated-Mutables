// Copyright 2012 Olivier Gillet, 2015 Tim Churches
//
// Author: Olivier Gillet (ol.gillet@gmail.com)
// Modifications: Tim Churches (tim.churches@gmail.com)
// Modifications may be determined by examining the differences between the last commit 
// by Olivier Gillet (pichenettes) and the HEAD commit at 
// https://github.com/timchurches/Mutated-Mutables/tree/master/braids 
//
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

#include <stm32f10x_conf.h>

#include <algorithm>

#include "stmlib/utils/dsp.h"
#include "stmlib/utils/ring_buffer.h"
#include "stmlib/system/system_clock.h"
#include "stmlib/system/uid.h"

#include "braids/drivers/adc.h"
#include "braids/drivers/dac.h"
#include "braids/drivers/debug_pin.h"
#include "braids/drivers/gate_input.h"
#include "braids/drivers/internal_adc.h"
#include "braids/drivers/system.h"
#include "braids/envelope.h"
#include "braids/macro_oscillator.h"
#include "braids/vco_jitter_source.h"
#include "braids/ui.h"

using namespace braids;
using namespace std;
using namespace stmlib;

const size_t kNumBlocks = 4;
const size_t kBlockSize = 24;

MacroOscillator osc;
Envelope envelope;  // first envelope/LFO 
Envelope envelope2; // second envelope/LFO 
Adc adc;
Dac dac;
DebugPin debug_pin;
GateInput gate_input;
InternalAdc internal_adc;
System sys;
VcoJitterSource jitter_source;
Ui ui;

size_t current_sample;
volatile size_t playback_block;
volatile size_t render_block;
int16_t audio_samples[kNumBlocks][kBlockSize];
uint8_t sync_samples[kNumBlocks][kBlockSize];

bool trigger_detected_flag;
volatile bool trigger_flag;
uint16_t trigger_delay;
static int32_t sh_pitch;


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

extern "C" {

void SysTick_Handler() {
  ui.Poll();
}

void TIM1_UP_IRQHandler(void) {
  if (!(TIM1->SR & TIM_IT_Update)) {
    return;
  }
  TIM1->SR = (uint16_t)~TIM_IT_Update;
  
  dac.Write(audio_samples[playback_block][current_sample] + 32768);
  
  bool trigger_detected = gate_input.raised();
  sync_samples[playback_block][current_sample] = trigger_detected;
  trigger_detected_flag = trigger_detected_flag | trigger_detected;

  current_sample = current_sample + 1;
  if (current_sample >= kBlockSize) {
     current_sample = 0;
     playback_block = (playback_block + 1) % kNumBlocks;
  }  
  
  bool adc_scan_cycle_complete = adc.PipelinedScan();
  if (adc_scan_cycle_complete) {
    ui.UpdateCv(adc.channel(0), adc.channel(1), adc.channel(2), adc.channel(3));
    if (trigger_detected_flag) {
      trigger_delay = settings.trig_delay()
          ? (1 << settings.trig_delay()) : 0;
      ++trigger_delay;
      trigger_detected_flag = false;
    }
    if (trigger_delay) {
      --trigger_delay;
      if (trigger_delay == 0) {
        trigger_flag = true;
      }
    }
  }
}

}

void Init() {
  sys.Init(F_CPU / 96000 - 1, true);
  settings.Init();
  ui.Init();
  system_clock.Init();
  adc.Init(false);
  gate_input.Init();
  // debug_pin.Init();
  dac.Init();
  osc.Init();
  internal_adc.Init();
  
  for (size_t i = 0; i < kNumBlocks; ++i) {
    fill(&audio_samples[i][0], &audio_samples[i][kBlockSize], 0);
    fill(&sync_samples[i][0], &sync_samples[i][kBlockSize], 0);
    for (size_t j = 0; j < kBlockSize; ++j) {
       audio_samples[i][j] = j * 2730;
    }
  }
  playback_block = kNumBlocks / 2;
  render_block = 0;
  current_sample = 0;
  
  sh_pitch = 69 << 7;
     
  envelope.Init();
  envelope2.Init();
  jitter_source.Init(GetUniqueId(1));
  sys.StartTimers();
}

const uint16_t bit_reduction_masks[] = {
    0xc000,
    0xe000,
    0xf000,
    0xf800,
    0xff00,
    0xfff0,
    0xffff };

const uint16_t decimation_factors[] = { 24, 12, 6, 4, 3, 2, 1 };

// table of log2 values for harmonic series quantisation, generated by the
// following R code: round(log2(1:16)*1024)
const uint16_t log2_table[] = { 0, 1024, 1623, 2048, 2378, 2647, 2875, 3072, 3246,
                                3402, 3542, 3671, 3789, 3899, 4001, 4096 };
                                
void RenderBlock() {
  static uint16_t previous_pitch_adc_code = 0;
  static uint16_t previous_fm_adc_code = 0;
  static int32_t previous_pitch = 0;
  static int32_t metaseq_pitch_delta = 0;
  static int32_t previous_shape = 0;
  static uint8_t metaseq_div_counter = 0;
  static uint8_t metaseq_steps_index = 0;
  static int8_t metaseq_index = 0;
  static bool current_mseq_dir = true;
  static uint8_t mod1_sync_index = 0;
  static uint8_t mod2_sync_index = 0;
  static uint8_t metaseq_parameter = 0;
  static uint8_t turing_div_counter = 0;
  static uint8_t turing_bit_position = 0;
  static uint32_t turing_shift_register = 0;
  static int32_t turing_pitch_delta = 0;
  
  // debug_pin.High();

  uint8_t meta_mod = settings.GetValue(SETTING_META_MODULATION); // FMCV setting, in fact
  uint8_t modulator1_mode = settings.GetValue(SETTING_MOD1_MODE);
  uint8_t modulator2_mode = settings.GetValue(SETTING_MOD2_MODE);

  // use FM CV data for env params if envelopes or LFO modes are enabled
  // Note, we invert the parameter if in LFO mode, so higher voltages produce 
  // higher LFO frequencies
  uint32_t env_param = uint32_t (settings.GetValue(SETTING_MOD1_RATE));
  uint32_t env_a = 0;
  uint32_t env_d = 0;
  // add the external voltage to this.
  // scaling this by 32 seems about right for 0-5V modulation range.
  if (meta_mod == 2 || meta_mod == 3) {
	 env_param += settings.adc_to_fm(adc.channel(3)) >> 5;
  }

  // Clip at zero and 127
  if (env_param < 0) {
	 env_param = 0 ;
  } else if (env_param > 127) {
	 env_param = 127 ;
  } 
  // Invert if in LFO mode, so higher CVs create higher LFO frequency.
  if (modulator1_mode == 1 && settings.rate_inversion()) {
	 env_param = 127 - env_param ;
  }  

  // attack and decay parameters, default to FM voltage reading.
  // These are ratios of attack to decay, from A/D = 0 to 127
  env_a = ((1 + settings.GetValue(SETTING_MOD1_AD_RATIO)) * env_param * 2) >> 8; 
  env_d = ((128 - settings.GetValue(SETTING_MOD1_AD_RATIO)) * env_param * 2) >> 8;  

  // now set the attack and decay parameters 
  // using the modified attack and decay values
  envelope.Update(env_a, env_d, 0, 0);  

  // Render envelope in LFO mode, or not
  // envelope 1
  uint8_t modulator1_attack_shape = settings.GetValue(SETTING_MOD1_ATTACK_SHAPE);
  uint8_t modulator1_decay_shape = settings.GetValue(SETTING_MOD1_DECAY_SHAPE);
  uint16_t ad_value = 0 ;
  if (modulator1_mode == 1) { 
	  // LFO mode
	  ad_value = envelope.Render(true, modulator1_attack_shape, modulator1_decay_shape);
  }
  else if (modulator1_mode > 1){
	  // envelope mode
	  ad_value = envelope.Render(false, modulator1_attack_shape, modulator1_decay_shape);
  }

  // TO-DO: instead of repeating code, use an array for env params and a loop!
  // Note: tried in branch envelope-tidy-up, but resulted in bigger compiled size
  uint32_t env2_param = uint32_t (settings.GetValue(SETTING_MOD2_RATE));
  uint32_t env2_a = 0;
  uint32_t env2_d = 0;
  // add the external voltage to this.
  // scaling this by 32 seems about right for 0-5V modulation range.
  if (meta_mod == 2 || meta_mod == 4) {
	 env2_param += settings.adc_to_fm(adc.channel(3)) >> 5;
  }
  // Add cross-modulation
  int8_t mod1_mod2_depth = settings.GetValue(SETTING_MOD1_MOD2_DEPTH);
  if (mod1_mod2_depth) {
	env2_param +=  (ad_value * mod1_mod2_depth) >> 18;
  }
  // Clip at zero and 127
  if (env2_param < 0) { 
	 env2_param = 0 ;
  } else if (env2_param > 127) {
	 env2_param = 127 ;
  } 
  // Invert if in LFO mode, so higher CVs create higher LFO frequency.
  if (modulator2_mode == 1 && settings.rate_inversion()) { 
	 env2_param = 127 - env2_param ;
  }  

  // These are ratios of attack to decay, from A/D = 0 to 127
  env2_a = ((1 + settings.GetValue(SETTING_MOD2_AD_RATIO)) * env2_param * 2) >> 8; 
  env2_d = ((128 - settings.GetValue(SETTING_MOD2_AD_RATIO)) * env2_param * 2) >> 8;  
 
  // now set the attack and decay parameters 
  // using the modified attack and decay values
  envelope2.Update(env2_a, env2_d, 0, 0);  

  // Render envelope in LFO mode, or not
  // envelope 2
  uint8_t modulator2_attack_shape = settings.GetValue(SETTING_MOD2_ATTACK_SHAPE);
  uint8_t modulator2_decay_shape = settings.GetValue(SETTING_MOD2_DECAY_SHAPE);
  uint16_t ad2_value = 0 ;
  if (modulator2_mode == 1) { 
	  // LFO mode
	  ad2_value = envelope2.Render(true, modulator2_attack_shape, modulator2_decay_shape);
  }
  else if (modulator2_mode > 1) {
	  // envelope mode
	  ad2_value = envelope2.Render(false, modulator2_attack_shape, modulator2_decay_shape);
  }

  // meta-sequencer
  uint8_t metaseq_length = settings.GetValue(SETTING_METASEQ);
  if (trigger_flag && metaseq_length) {
     ++metaseq_div_counter;
     if (metaseq_div_counter >= settings.GetValue(SETTING_METASEQ_CLOCK_DIV)) {
        metaseq_div_counter = 0;
	    ++metaseq_steps_index;
	    uint8_t metaseq_direction = settings.GetValue(SETTING_METASEQ_DIRECTION);
	    if (metaseq_steps_index >= (settings.metaseq_step_length(metaseq_index))) { 
	       metaseq_steps_index = 0;
		   if (metaseq_direction == 0) {
		      // looping
		      ++metaseq_index;
		      if (metaseq_index > metaseq_length) { 
		         metaseq_index = 0;
		      }
		   } else if (metaseq_direction == 1) {
		      // swing
		      if (current_mseq_dir) {
		         // ascending
			     ++metaseq_index;
			     if (metaseq_index >= metaseq_length) {
			        metaseq_index = metaseq_length; 
				    current_mseq_dir = !current_mseq_dir;
			     }
		      } else {
			     // descending
			     --metaseq_index;
			     if (metaseq_index == 0) { 
			        current_mseq_dir = !current_mseq_dir;
			      }
		       }             
		   } else if (metaseq_direction == 2) {
		     // random
		     metaseq_index = uint8_t(Random::GetWord() >> 29);
		   }
        }
	    MacroOscillatorShape metaseq_current_shape = settings.metaseq_shape(metaseq_index);
	    osc.set_shape(metaseq_current_shape);
	    ui.set_meta_shape(metaseq_current_shape);
	    metaseq_pitch_delta = settings.metaseq_note(metaseq_index) * 128;
        metaseq_parameter = settings.metaseq_parameter(metaseq_index) ;
     }
  } // end meta-sequencer

  // uint8_t c_major_scale[16] = { 0, 2, 4, 5, 7, 9, 11, 12, 14, 16, 17, 19, 21, 23, 24, 26 };
  // uint8_t c_major_scale[16] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26 };
  uint8_t c_major_scale[16] = { 0, 2, 4, 7, 9, 12, 14, 16, 19, 21, 24, 26, 28, 31, 33, 35 };

  // Turing machine
  uint8_t turing_length = settings.GetValue(SETTING_TURING_LENGTH);
  if (trigger_flag && turing_length) {
     ++turing_div_counter;
     if (turing_div_counter >= settings.GetValue(SETTING_TURING_CLOCK_DIV)) {
        turing_div_counter = 0;
        ++turing_bit_position;
        // decide whether to flip the MSB
        if (turing_bit_position >= turing_length) {
           turing_bit_position = 0;
           if (settings.turing_init()) {
              turing_shift_register = Random::GetWord();
           }
           if ((Random::GetWord() >> 25) < settings.GetValue(SETTING_TURING_PROB)) {
              // bit-flip the MSB
              turing_shift_register ^= 1 << (turing_length - 1);
           }
        }
        // read the LSB
        uint32_t turing_lsb = turing_shift_register & 1;
        // rotate the shift register
        turing_shift_register = turing_shift_register >> 1;
        // add back the LSB into the MSB postion
        turing_shift_register = turing_shift_register | turing_lsb << (turing_length - 1);       
        // read the window and calculate pitch increment
        uint8_t turing_value = (turing_shift_register & 127) >> (7 - settings.GetValue(SETTING_TURING_WINDOW));        
        turing_pitch_delta = c_major_scale[turing_value] * 128 ;
     }
  } // end Turing machine

  // modulate timbre
  int32_t parameter_1 = adc.channel(0) << 3; 
  if (modulator1_mode == 2) {
	 parameter_1 -= (ad_value * settings.mod1_timbre_depth()) >> 9;
  } else {
	 parameter_1 += (ad_value * settings.mod1_timbre_depth()) >> 9;
  }  
  if (modulator2_mode == 2) {  
     parameter_1 -= (ad2_value * settings.mod2_timbre_depth()) >> 9;
  } else {
     parameter_1 += (ad2_value * settings.mod2_timbre_depth()) >> 9;
  }
  // scale the gain by the meta-sequencer parameter if applicable
  if (metaseq_length && (settings.GetValue(SETTING_METASEQ_PARAMETER_DEST) & 1)) {
     parameter_1 = (parameter_1 * metaseq_parameter) >> 7;
  }
  // clip
  if (parameter_1 > 32767) {
	parameter_1 = 32767;
  } else if (parameter_1 < 0) {
	parameter_1 = 0;
  }

  // modulate colour
  int32_t parameter_2 = adc.channel(1) << 3; 
  if (modulator1_mode == 2) {
	 parameter_2 -= (ad_value * settings.mod1_color_depth()) >> 9;
  } else {
	 parameter_2 += (ad_value * settings.mod1_color_depth()) >> 9;
  }
  if (modulator2_mode == 2) {  
	 parameter_2 -= (ad2_value * settings.mod2_color_depth()) >> 9;
  } else {
	 parameter_2 += (ad2_value * settings.mod2_color_depth()) >> 9;
  }
  // scale the gain by the meta-sequencer parameter if applicable
  if (metaseq_length && (settings.GetValue(SETTING_METASEQ_PARAMETER_DEST) & 2)) {
     parameter_2 = (parameter_2 * metaseq_parameter) >> 7;
  }
  // clip
  if (parameter_2 > 32767) {
	parameter_2 = 32767;
  } else if (parameter_2 < 0) {
	parameter_2 = 0;
  }
  
  // set the timbre and color parameters on the oscillator
  osc.set_parameters(uint16_t(parameter_1), uint16_t(parameter_2));

  // meta_modulation no longer a boolean  
  // meta-sequencer over-rides FMCV=META and the WAVE setting
  if (!metaseq_length) {
	  if (meta_mod == 1) {
		int32_t shape = adc.channel(3);
		shape -= settings.data().fm_cv_offset;
		if (shape > previous_shape + 2 || shape < previous_shape - 2) {
		  previous_shape = shape;
		} else {
		  shape = previous_shape;
		}
		shape = MACRO_OSC_SHAPE_LAST * shape >> 11;
		shape += settings.shape();
		if (shape >= MACRO_OSC_SHAPE_LAST_ACCESSIBLE_FROM_META) {
			shape = MACRO_OSC_SHAPE_LAST_ACCESSIBLE_FROM_META;
		} else if (shape <= 0) {
		  shape = 0;
		}
		MacroOscillatorShape osc_shape = static_cast<MacroOscillatorShape>(shape);
		osc.set_shape(osc_shape);
		ui.set_meta_shape(osc_shape);
	  } else {
		osc.set_shape(settings.shape());
	  }
  } 
  
  // Apply hysteresis to ADC reading to prevent a single bit error to move
  // the quantized pitch up and down the quantization boundary.
  uint16_t pitch_adc_code = adc.channel(2);
  if (settings.pitch_quantization()) {
    if ((pitch_adc_code > previous_pitch_adc_code + 4) ||
        (pitch_adc_code < previous_pitch_adc_code - 4)) {
      previous_pitch_adc_code = pitch_adc_code;
    } else {
      pitch_adc_code = previous_pitch_adc_code;
    }
  }
  
  int32_t pitch = settings.adc_to_pitch(pitch_adc_code);

  // Sample and hold pitch if enabled
  if (settings.pitch_sample_hold()) {
     if (trigger_flag) {
        sh_pitch = pitch;
     }
     pitch = sh_pitch; 
  }
  
  // add vibrato from modulators 1 and 2 before or after quantisation
  uint8_t mod1_vibrato_depth = settings.GetValue(SETTING_MOD1_VIBRATO_DEPTH); // 0 to 127
  uint8_t mod2_vibrato_depth = settings.GetValue(SETTING_MOD2_VIBRATO_DEPTH); // 0 to 127
  bool mod1_mod2_vibrato_depth = settings.mod1_mod2_vibrato_depth();
  bool quantize_vibrato = settings.quantize_vibrato();
  int32_t pitch_delta1 = 0 ;
  int32_t pitch_delta2 = 0 ;

  // calculate vibrato amount, vibrato should be bipolar
  if (mod1_vibrato_depth) {
     pitch_delta1 = ((ad_value - 32767) * mod1_vibrato_depth) >> 11 ; 
  }
    
  // mod1 envelope mediates the degree of vibrato from mod2, or not.
  if (mod2_vibrato_depth) {
     pitch_delta2 = ((ad2_value - 32767) * mod2_vibrato_depth) >> 11;
     if (mod1_mod2_vibrato_depth) {
        pitch_delta2 = (pitch_delta2 * ad_value) >> 16;
     }
  }

  if (quantize_vibrato) {     
	  if (modulator1_mode == 2) {
		 pitch -= pitch_delta1; 
	  } else {  
		 pitch += pitch_delta1; 
	  }    

	  if (modulator2_mode == 2) {
		 pitch -= pitch_delta2;
	  } else {
		 pitch += pitch_delta2;
	  }        
  }
  
  if (settings.pitch_quantization() == PITCH_QUANTIZATION_QUARTER_TONE) {
    pitch = (pitch + 32) & 0xffffffc0;
  } else if (settings.pitch_quantization() == PITCH_QUANTIZATION_SEMITONE) {
    pitch = (pitch + 64) & 0xffffff80;
  }

  // add FM
  if (meta_mod == 0) {
    pitch += settings.adc_to_fm(adc.channel(3));
  }
  
  pitch += internal_adc.value() >> 8;

  // or harmonic intervals 
  if (meta_mod == 6) {
     // Apply hysteresis to ADC reading to prevent a single bit error to move
     // the quantized pitch up and down the quantization boundary.
     uint16_t fm_adc_code = adc.channel(3);
     if ((fm_adc_code > previous_fm_adc_code + 4) ||
         (fm_adc_code < previous_fm_adc_code - 4)) {
        previous_fm_adc_code = fm_adc_code;
     } else {
        fm_adc_code = previous_fm_adc_code;
     }
     int32_t harmonic_multiplier = settings.adc_to_fm(fm_adc_code) >> 8;
     if (harmonic_multiplier < -15) {
	    harmonic_multiplier = -15;
     } else if (harmonic_multiplier > 15) {
        harmonic_multiplier = 15;
     }
     if (harmonic_multiplier > 0) {
        pitch += (1536 * log2_table[harmonic_multiplier - 1]) >> 10;
     } else if (harmonic_multiplier < 0) {
        pitch -= (1536 * log2_table[-1 - harmonic_multiplier]) >> 10;
     }
  }
  
  // Check if the pitch has changed to cause an auto-retrigger
  int32_t pitch_delta = pitch - previous_pitch;
  if (settings.data().auto_trig &&
      (pitch_delta >= 0x40 || -pitch_delta >= 0x40)) {
    trigger_detected_flag = true;
  }
  previous_pitch = pitch;

  // Or add vibrato here
  if (!quantize_vibrato) {
	  if (modulator1_mode == 2) {
		 pitch -= pitch_delta1; 
	  } else {  
		 pitch += pitch_delta1; 
	  }    

	  if (modulator2_mode == 2) {
		 pitch -= pitch_delta2;
	  } else {
		 pitch += pitch_delta2;
	  }        
  }

  // jitter depth now settable and voltage controllable.
  // TO-DO jitter still causes pitch to sharpen slightly - why?
  int32_t vco_drift = settings.vco_drift();
  if (meta_mod == 7 || meta_mod == 10 || meta_mod == 11 || meta_mod == 13) {
     vco_drift += settings.adc_to_fm(adc.channel(3)) >> 6;
  } 
  if (vco_drift) {
     if (vco_drift < 0) {
	    vco_drift = 0 ;
     } else if (vco_drift > 127) {
        vco_drift = 127;
     }
    // now apply the jitter
    pitch +=  (jitter_source.Render(adc.channel(1) << 3) >> 8) * vco_drift;
  }

  if (metaseq_length) {
     pitch += metaseq_pitch_delta;
  }

  if (turing_length) {
     pitch += turing_pitch_delta;
  }

  // add software fine tune
  pitch += settings.fine_tune();
  
  // clip the pitch to prevent bad things from happening.
  if (pitch > 32767) {
    pitch = 32767;
  } else if (pitch < 0) {
    pitch = 0;
  }
  
  osc.set_pitch(pitch + settings.pitch_transposition());

  if (trigger_flag) {
    osc.Strike();
    // reset internal modulator phase if mod1_sync or mod2_sync > 0
    // and if a trigger counter for each = the setting of mod1_sync
    // or mod2_sync (defaults to 1 thus every trigger).
    if (settings.GetValue(SETTING_MOD1_SYNC)) {
       ++mod1_sync_index;
       if (mod1_sync_index >= settings.GetValue(SETTING_MOD1_SYNC)) {
          envelope.Trigger(ENV_SEGMENT_ATTACK);
          mod1_sync_index = 0 ;
       }
    }
    if (settings.GetValue(SETTING_MOD2_SYNC)) {
       ++mod2_sync_index;
       if (mod2_sync_index >= settings.GetValue(SETTING_MOD2_SYNC)) {
          envelope2.Trigger(ENV_SEGMENT_ATTACK);
          mod2_sync_index = 0 ;
       }
    }
    ui.StepMarquee(); // retained because this is what causes the CV tester to blink on each trigger
    trigger_flag = false;
  }

  uint8_t* sync_buffer = sync_samples[render_block];
  int16_t* render_buffer = audio_samples[render_block];
  if (!settings.osc_sync()) {
    // Disable hardsync when oscillator sync disabled.
    memset(sync_buffer, 0, kBlockSize);
   }

  osc.Render(sync_buffer, render_buffer, kBlockSize);

  // gain is a weighted sum of the envelope/LFO levels  
  uint32_t mod1_level_depth = uint32_t(settings.mod1_level_depth());
  uint32_t mod2_level_depth = uint32_t(settings.mod2_level_depth());
  int32_t gain = settings.initial_gain(); 
  // add external CV if FMCV used for level
  if (meta_mod == 5) {
     gain += settings.adc_to_fm(adc.channel(3)) << 4; // was 3 
  } 
  // Gain mod by modulator 1
  if (modulator1_mode  && modulator1_mode < 3) {
     // subtract from full gain if LFO-only modes (mode==1) or Env- modes (mode==2)
     gain -= (ad_value * mod1_level_depth) >> 8;
  } else if (modulator1_mode == 3) {
     gain += (ad_value * mod1_level_depth) >> 8;
  }
  // Gain mod by modulator 2
  if (modulator2_mode  && modulator2_mode < 3) {
     // subtract from full gain if LFO-only modes (mode==1) or Env- modes (mode==2)
     gain -= (ad2_value * mod2_level_depth) >> 8;
  } else if (modulator2_mode == 3) {
     gain += (ad2_value * mod2_level_depth) >> 8;
  }
  // scale the gain by the meta-sequencer parameter if applicable
  if (metaseq_length && (settings.GetValue(SETTING_METASEQ_PARAMETER_DEST) & 4)) {
     gain = (gain * metaseq_parameter) >> 7;
  }
  // clip the gain  
  if (gain > 65535) {
      gain = 65535;
  }
  else if (gain < 0) {
      gain = 0;
  }

  // Voltage control of bit crushing
  uint8_t bits_value = settings.resolution();
  if (meta_mod == 8 || meta_mod == 10 || meta_mod >= 12 ) {
     bits_value -= settings.adc_to_fm(adc.channel(3)) >> 9;
     if (bits_value < 0) {
	    bits_value = 0 ;
     } else if (bits_value > 6) {
        bits_value = 6;
     }
  }

  // Voltage control of sample rate decimation
  uint8_t sample_rate_value = settings.data().sample_rate;
  if (meta_mod == 9 || meta_mod >= 11 ) {
     sample_rate_value -= settings.adc_to_fm(adc.channel(3)) >> 9;
     if (sample_rate_value < 0) {
	    sample_rate_value = 0 ;
     } else if (sample_rate_value > 6) {
        sample_rate_value = 6;
     }
  }
     
  // Copy to DAC buffer with sample rate and bit reduction applied.
  int16_t sample = 0;
  size_t decimation_factor = decimation_factors[sample_rate_value];  
  uint16_t bit_mask = bit_reduction_masks[bits_value];
  for (size_t i = 0; i < kBlockSize; ++i) {
    if ((i % decimation_factor) == 0) {
       sample = render_buffer[i] & bit_mask;
    }
    render_buffer[i] = static_cast<int32_t>(sample) * gain >> 16;
  }
  render_block = (render_block + 1) % kNumBlocks;
  // debug_pin.Low();
}

int main(void) {
  Init();
  while (1) {
    while (render_block != playback_block) {
      RenderBlock();
    }
    ui.DoEvents();
  }
}
