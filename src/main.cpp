#include <Arduino.h>
#include "AudioConfigLocal.h"
#include <AudioTools.h>
#include <AudioTools/AudioLibs/AudioRealFFT.h>
#include <AudioTools/AudioLibs/AudioSourceSPIFFS.h>
#include <CircularBuffer.hpp>
#include <AsyncTimer.h>

// A/D Converter PINs
#define AD_MCLK 0
#define AD_LRCK 1
#define AD_SCLK 2
#define AD_SDIN 3

// D/A Converter PINs
#define DA_MCLK 4
#define DA_LRCK 5
#define DA_SCLK 6
#define DA_SDIN 7

// I/O PINs
#define RX_LED 8
#define TX_LED 9
#define ANNONCE_BTN 10

// CONST
#define CTCSS_LVL 0.2

// Définition des variables globales
AudioInfo info(22050, 1, 16);
I2SStream in;
I2SStream out;
SineWaveGenerator<int16_t> ctcss_sine;
GeneratedSoundStream<int16_t> ctcss(ctcss_sine); 
OutputMixer<int16_t> mixer(out, 3); 
BufferedStream MixerIn1(1024,mixer);
BufferedStream MixerIn2(1024,mixer);
//BufferedStream MixerIn3(1024,mixer);
VolumeMeter volumeMeter(MixerIn1);
AudioRealFFT fft;
MultiOutput multiOutput(volumeMeter,fft);
CircularBuffer <AudioFFTResult,10> FFTBuf;
AsyncTimer t;
WAVDecoder decoder;
AudioSourceSPIFFS source("/",".wav");
AudioPlayer player(source,MixerIn2,decoder);
StreamCopy InCopier(multiOutput, in, 1024);
StreamCopy ctcss_copier(mixer,ctcss);

enum Mode
{
  IDLE,
  ANNONCE_DEB,
  REPEATER,
  ANNONCE_FIN
};

Mode Etat = IDLE;
Mode LastEtat=IDLE;

bool CD = false;
float mag_ref = 10000000.0;
float SeuilSquelch = -30.0; //Squelch threshold
uint8_t Counter = 0;

// Proto
void fftResult(AudioFFTBase &fft);
void OnTimer ();
bool Is1750Detected ();
void Actions (const Mode& pState);
int lastState = HIGH;
int currentState;

/// @brief 
/// @param  
void setup(void)
{
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);
 
  //
  // Configure in stream
  //
  auto configin = in.defaultConfig(RX_MODE);
  configin.copyFrom(info);
  configin.i2s_format = I2S_STD_FORMAT;
  configin.is_master = true;
  configin.port_no = 0;
  configin.pin_ws = AD_LRCK;                        // LRCK
  configin.pin_bck = AD_SCLK;                       // SCLK
  configin.pin_data = AD_SDIN;                      // SDOUT
  configin.pin_mck = AD_MCLK;
  in.begin(configin);
  
  //
  // Configure out stream
  //
  auto configout = out.defaultConfig(TX_MODE);
  configout.copyFrom(info);
  configout.i2s_format = I2S_STD_FORMAT;
  configout.is_master = true;
  configout.port_no = 1;
  configout.pin_ws = DA_LRCK;                        // LRCK
  configout.pin_bck = DA_SCLK;                       // SCLK
  configout.pin_data = DA_SDIN;                      // SDOUT
  configout.pin_mck = DA_MCLK;
  out.begin(configout);

  //
  // Configure FFT
  //
  auto tcfg = fft.defaultConfig();
  tcfg.length = 512;
  tcfg.copyFrom(info);
  //tcfg.window_function = new BlackmanHarris;
  tcfg.callback = &fftResult;
  fft.begin(tcfg);

  //
  // Configure Volume Meter
  //
  volumeMeter.begin(info);

  //
  // Configure Player
  //
  player.setAudioInfo(info);
  player.setSilenceOnInactive(true);
  player.begin(0,false);


  //
  // CTCSS Generator
  //
  ctcss_sine.setFrequency(62.5);
  ctcss.begin(info);
  //
  // Configure Mixer
  //
  mixer.begin(1024);

  //
  // Configure I/O
  //
  pinMode(RX_LED, OUTPUT);
  pinMode(TX_LED, OUTPUT);
  pinMode(ANNONCE_BTN, INPUT_PULLUP);


  //
  // Configure Slow Timer
  //
  t.setInterval(OnTimer, 1000);

  Actions(IDLE);

}

/// @brief Detect a 1750 Hz tone 
/// @return true is 1750 Hz is detected
bool Is1750Detected ()
{
  float tolerance = 100; // Tolérance de détection
  // Si il n'y a pas assez de données dans le buffer on passe
  if (FFTBuf.size() <10 ) return false;
  using index_t = decltype(FFTBuf)::index_t;
	for (index_t i = 0; i < FFTBuf.size(); i++)
  {

      //Serial.println( "Freq = " + String(FFTBuf[i].frequency) + " Magnitude = "+ String(FFTBuf[i].magnitude));
			if (
        (FFTBuf[i].frequency < (1750.0 + tolerance)) &&
        (FFTBuf[i].frequency > (1750.0-tolerance)) &&
        (FFTBuf[i].magnitude > 100000.0))
      {
        return true;
      }
	}
  return false;
}

void Actions (const Mode& pState)
{
  switch (pState)
  {
    case IDLE:
    {
      LOGW("IDLE");
      digitalWrite(RX_LED, HIGH);
      digitalWrite(TX_LED,LOW);
      mixer.setWeight(0,0.0);
      mixer.setWeight(1,0.0);
      mixer.setWeight(2,0.0);
      break;
    }
    case ANNONCE_DEB:
    {
      LOGW("Annonce début");
      digitalWrite(RX_LED, LOW);
      digitalWrite(TX_LED,HIGH);
      //mute input sound still playing welcome
      mixer.setWeight(0,0.0);
      mixer.setWeight(1,1.0);
      mixer.setWeight(2,CTCSS_LVL);
      player.begin(1);
      player.setAutoNext(false);
      break;
    }
    case REPEATER:
    {
      LOGW("Repeater");
      digitalWrite(RX_LED, LOW);
      digitalWrite(TX_LED,HIGH);
      mixer.setWeight(0,1.0);
      mixer.setWeight(1,0.0);
      mixer.setWeight(2,CTCSS_LVL);
      break;
    }
    case ANNONCE_FIN:
    {
      LOGW("Annonce Fin");
      digitalWrite(RX_LED, LOW);
      digitalWrite(TX_LED,HIGH);
      mixer.setWeight(0,0.0);
      mixer.setWeight(1,1.0);
      mixer.setWeight(2,CTCSS_LVL);
      player.begin(0);
      player.setAutoNext(false);
      break;
    }
  }
  Etat = pState;
}

/// @brief Sequence is reviewed each second
void OnTimer ()
{
  if (volumeMeter.volumeDB() > SeuilSquelch)
  {
    Serial.println("Volume = " + String(volumeMeter.volumeDB()));
    CD = true;
  }
  else
  {
    CD =false;
  }
  //Grafcet
  switch (Etat)
  {
    case IDLE:
    {
      if (Is1750Detected() && CD)
      {
        Counter ++;
      }
      else
      {
        Counter = 0;
      }
      if (Counter >= 2)
      {
        Actions(ANNONCE_DEB);
        Counter=0;
      }
      break;
    }
    case ANNONCE_DEB:
    {
      Counter++;
      if (Counter > 3)
      {
        Actions(REPEATER);
        Counter=0;
      }
      break;
    }
    case REPEATER:
    {
      if (!CD) Counter ++;
      else Counter = 0;
      if (Counter > 10)
      {
        Actions(ANNONCE_FIN);
        Counter=0;
      }
      break;
    }
    case ANNONCE_FIN:
    {
      Counter++;
      if (Counter > 3)
      {
        Actions(IDLE);
        Counter=0;
      }
      break;
    }
  }
}

//
// Fill a circular buffer to analyse freq
//
void fftResult(AudioFFTBase &fft)
{
    float diff;
    float CarriageAvg=0.0;
    AudioFFTResult Score[5];
    fft.resultArray(Score);
    for (AudioFFTResult Line : Score)
    {
      CarriageAvg += Line.magnitude / 5;
      FFTBuf.push(Line);
    }
}

//
// Main Loop
//
void loop()
{
  currentState = digitalRead(ANNONCE_BTN);
  if (lastState == LOW && currentState == HIGH)
  {
    Actions(ANNONCE_DEB);
  }
  lastState = currentState;
  t.handle();
  // Audio Processing
  InCopier.copy();
  player.copy();
  ctcss_copier.copy();
  if (mixer.size() > 0)
  {
    mixer.flushMixer();
  }
}