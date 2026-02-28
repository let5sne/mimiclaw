# Voice Interaction Implementation Roadmap

## Overview

This document outlines the implementation plan for adding complete voice interaction to MimiClaw, including wake word detection, speech recognition (ASR), and text-to-speech (TTS).

## Architecture

```
┌─────────────┐
│  Microphone │
└──────┬──────┘
       │ I2S Audio Stream
       ▼
┌─────────────────────┐
│  Wake Word Detector │ (ESP-SR)
│  "Hi Mimi"          │
└──────┬──────────────┘
       │ Wake Event
       ▼
┌─────────────────────┐
│  Audio Recorder     │
│  (VAD + Buffering)  │
└──────┬──────────────┘
       │ Audio Data
       ▼
┌─────────────────────┐
│  Cloud ASR Service  │ (Whisper API / Google STT)
│  Audio → Text       │
└──────┬──────────────┘
       │ Text
       ▼
┌─────────────────────┐
│  Agent Loop         │ (Existing)
│  Claude Processing  │
└──────┬──────────────┘
       │ Response Text
       ▼
┌─────────────────────┐
│  Cloud TTS Service  │ (OpenAI TTS / Google TTS)
│  Text → Audio       │
└──────┬──────────────┘
       │ Audio Data
       ▼
┌─────────────────────┐
│  Audio Player       │
│  (I2S Speaker)      │
└─────────────────────┘
```

## Implementation Phases

### Phase 1: Basic Audio I/O ✅ (Current)

**Status**: Framework created

**Components**:
- [x] Audio module interface (`audio/audio.h`)
- [x] I2S microphone driver
- [x] I2S speaker driver
- [x] Basic audio playback
- [x] Volume control
- [x] Configuration system

**Files**:
- `main/audio/audio.h` - Audio interface
- `main/audio/audio.c` - Basic I2S implementation
- `main/mimi_config.h` - Audio configuration

### Phase 2: Wake Word Detection (TODO)

**Goal**: Detect "Hi Mimi" wake word using ESP-SR

**Tasks**:
1. Add ESP-SR component to project
   - Add to `idf_component.yml` or download manually
   - Configure ESP-SR in `sdkconfig`

2. Integrate ESP-SR wake word detection
   - Create `audio/wake_word.c`
   - Load wake word model
   - Process audio stream for wake word
   - Trigger recording on detection

3. Update display on wake word
   - Show "Listening..." status
   - Visual feedback for user

**Dependencies**:
- ESP-SR library (from Espressif)
- Wake word model file (stored in SPIFFS)

**Estimated Effort**: 2-3 days

### Phase 3: Audio Recording & VAD (TODO)

**Goal**: Record user speech after wake word, detect silence

**Tasks**:
1. Implement Voice Activity Detection (VAD)
   - Detect speech start/end
   - Calculate audio energy/volume
   - Silence timeout (e.g., 2 seconds)

2. Audio buffering
   - Circular buffer for continuous recording
   - Save audio to memory/SPIFFS
   - Support multiple audio formats (PCM, WAV)

3. Audio preprocessing
   - Noise reduction (optional)
   - Automatic gain control
   - Resampling if needed

**Files to create**:
- `main/audio/vad.c` - Voice Activity Detection
- `main/audio/recorder.c` - Audio recording logic

**Estimated Effort**: 2-3 days

### Phase 4: Cloud ASR Integration (TODO)

**Goal**: Convert recorded audio to text using cloud service

**Options**:

#### Option A: OpenAI Whisper API (Recommended)
- **Pros**: High accuracy, supports multiple languages, easy API
- **Cons**: Requires API key, costs money
- **API**: `https://api.openai.com/v1/audio/transcriptions`

#### Option B: Google Speech-to-Text
- **Pros**: Very accurate, streaming support
- **Cons**: More complex setup, requires Google Cloud account

#### Option C: Local Whisper (ESP32-S3)
- **Pros**: No cloud dependency, no cost
- **Cons**: Very limited model size, slower, less accurate

**Tasks**:
1. Create ASR client module
   - HTTP client for API calls
   - Audio format conversion (to MP3/OGG if needed)
   - Handle API responses

2. Integrate with agent loop
   - Send transcribed text to agent
   - Handle errors gracefully

**Files to create**:
- `main/audio/asr_client.c` - ASR API client
- `main/audio/audio_encoder.c` - Audio format conversion (optional)

**Estimated Effort**: 2-3 days

### Phase 5: Cloud TTS Integration (TODO)

**Goal**: Convert agent response text to speech audio

**Options**:

#### Option A: OpenAI TTS API (Recommended)
- **Pros**: Natural voices, easy API, multiple voices
- **Cons**: Requires API key, costs money
- **API**: `https://api.openai.com/v1/audio/speech`

#### Option B: Google Text-to-Speech
- **Pros**: High quality, many voices and languages
- **Cons**: More complex setup

#### Option C: Local TTS (ESP32-S3)
- **Pros**: No cloud dependency
- **Cons**: Robotic voice, limited quality

**Tasks**:
1. Create TTS client module
   - HTTP client for API calls
   - Stream audio response
   - Handle chunked audio playback

2. Integrate with agent loop
   - Convert agent response to speech
   - Play audio through speaker
   - Update display status

**Files to create**:
- `main/audio/tts_client.c` - TTS API client
- `main/audio/audio_decoder.c` - Audio format decoding (MP3/OGG)

**Estimated Effort**: 2-3 days

### Phase 6: Voice Interaction State Machine (TODO)

**Goal**: Coordinate all voice components into smooth interaction flow

**States**:
1. **IDLE** - Waiting for wake word
2. **WAKE_DETECTED** - Wake word detected, start recording
3. **RECORDING** - Recording user speech
4. **PROCESSING_ASR** - Converting speech to text
5. **THINKING** - Agent processing request
6. **PROCESSING_TTS** - Converting response to speech
7. **SPEAKING** - Playing audio response
8. **ERROR** - Handle errors, return to IDLE

**Tasks**:
1. Create state machine
   - Define states and transitions
   - Handle timeouts and errors
   - Coordinate display updates

2. Integrate all components
   - Wake word → Recording → ASR → Agent → TTS → Playback
   - Error handling at each stage
   - User feedback (display, LED, beep)

**Files to create**:
- `main/audio/voice_assistant.c` - Main voice interaction coordinator

**Estimated Effort**: 3-4 days

### Phase 7: Testing & Optimization (TODO)

**Tasks**:
1. Test in various environments
   - Quiet room
   - Noisy environment
   - Different distances from microphone

2. Optimize performance
   - Reduce latency
   - Improve wake word accuracy
   - Optimize memory usage

3. Add configuration options
   - Adjustable wake word sensitivity
   - VAD threshold tuning
   - Audio quality settings

**Estimated Effort**: 2-3 days

## Hardware Requirements

### Minimum Setup
- ESP32-S3 with 8MB PSRAM
- I2S MEMS microphone (e.g., INMP441, SPH0645)
- I2S audio amplifier + speaker (e.g., MAX98357A)
- Power supply (USB or battery)

### Recommended Setup
- ESP32-S3 with 16MB Flash + 8MB PSRAM
- High-quality I2S microphone
- I2S DAC + amplifier (e.g., PCM5102 + PAM8403)
- Good quality speaker
- Optional: LED indicator for status

## Pin Configuration

Default pins (configurable in `mimi_secrets.h`):

**Microphone (I2S0)**:
- WS (LRCK): GPIO42
- SCK (BCLK): GPIO41
- SD (DIN): GPIO2

**Speaker (I2S1)**:
- WS (LRCK): GPIO15
- SCK (BCLK): GPIO16
- SD (DOUT): GPIO17

## API Keys Required

1. **Anthropic API Key** (existing) - For Claude agent
2. **OpenAI API Key** (new) - For Whisper ASR + TTS
   - Or Google Cloud credentials for Google services

## Memory Considerations

- Wake word model: ~200KB (SPIFFS)
- Audio buffer: ~64KB (PSRAM)
- ASR/TTS buffers: ~128KB (PSRAM)
- Total additional: ~400KB

ESP32-S3 with 8MB PSRAM should be sufficient.

## Cost Estimate (Cloud Services)

Using OpenAI APIs:
- Whisper ASR: $0.006 per minute
- TTS: $0.015 per 1000 characters

Example: 100 voice interactions per day
- Average 10 seconds speech input: $0.01/day
- Average 100 characters response: $0.15/day
- **Total: ~$5/month**

## Next Steps

1. **Immediate**: Test current display implementation
2. **Short-term**: Implement Phase 2 (Wake Word Detection)
3. **Medium-term**: Complete Phases 3-5 (Recording, ASR, TTS)
4. **Long-term**: Polish and optimize (Phase 6-7)

## References

- [ESP-SR Documentation](https://github.com/espressif/esp-sr)
- [OpenAI Whisper API](https://platform.openai.com/docs/guides/speech-to-text)
- [OpenAI TTS API](https://platform.openai.com/docs/guides/text-to-speech)
- [I2S Driver Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html)
