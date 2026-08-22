#include "spu.h"

#include <algorithm>

#include "savestate.h"

namespace {

// A voice's own registers, sixteen bytes of them, twenty-four times
// over from the base. Everything from 0x1F801D80 up is the control
// block shared by all of them.
constexpr u32 VOICE_STRIDE = 0x10;
constexpr u32 VOICE_END = Spu::BASE + Spu::VOICE_COUNT * VOICE_STRIDE;

constexpr u32 VOICE_VOLUME_LEFT = 0x0;
constexpr u32 VOICE_VOLUME_RIGHT = 0x2;
constexpr u32 VOICE_SAMPLE_RATE = 0x4;
constexpr u32 VOICE_START_ADDRESS = 0x6;
constexpr u32 VOICE_ADSR_LO = 0x8;
constexpr u32 VOICE_ADSR_HI = 0xA;
constexpr u32 VOICE_ADSR_VOLUME = 0xC;
constexpr u32 VOICE_REPEAT_ADDRESS = 0xE;

// The control block, by address.
constexpr u32 MAIN_VOLUME_LEFT = 0x1F801D80;
constexpr u32 MAIN_VOLUME_RIGHT = 0x1F801D82;
constexpr u32 KEY_ON = 0x1F801D88;
constexpr u32 KEY_OFF = 0x1F801D8C;
constexpr u32 NOISE_MODE = 0x1F801D94;
constexpr u32 ENDX = 0x1F801D9C;
constexpr u32 IRQ_ADDRESS = 0x1F801DA4;
constexpr u32 TRANSFER_ADDRESS = 0x1F801DA6;
constexpr u32 TRANSFER_FIFO = 0x1F801DA8;
constexpr u32 CONTROL = 0x1F801DAA;
constexpr u32 STATUS = 0x1F801DAE;
constexpr u32 CD_VOLUME_LEFT = 0x1F801DB0;
constexpr u32 CD_VOLUME_RIGHT = 0x1F801DB2;

// Bits of the control register. The transfer mode is the pair that
// matters most here: it is what the status register has to echo back
// before the sound library will go on.
constexpr u16 CONTROL_CD_AUDIO = 0x0001;
constexpr u16 CONTROL_TRANSFER_MODE = 0x0030;
constexpr u16 CONTROL_IRQ_ENABLE = 0x0040;
constexpr u16 CONTROL_UNMUTE = 0x4000;
constexpr u16 CONTROL_ENABLE = 0x8000;

// The low six bits of the control register are the "current mode",
// which the status register reports back once the SPU has adopted it.
constexpr u16 STATUS_MODE_MASK = 0x003F;
constexpr u16 STATUS_IRQ_FLAG = 1 << 6;
constexpr u16 STATUS_TRANSFER_REQUEST = 1 << 7;
constexpr u16 STATUS_DMA_WRITE_REQUEST = 1 << 8;
constexpr u16 STATUS_DMA_READ_REQUEST = 1 << 9;

// Transfer modes, in the two bits above: stop, manual, DMA write and
// DMA read, in that order. Only the two DMA modes are named, since
// they are the only ones the status register answers differently for —
// the other two ask the DMA controller for nothing.
constexpr u16 TRANSFER_DMA_WRITE = 2;
constexpr u16 TRANSFER_DMA_READ = 3;

// Software sets every address in units of eight bytes, since sample
// data is written in blocks of sixteen and never starts anywhere finer
// than this.
constexpr u32 ADDRESS_UNIT = 8;

// The second byte of an ADPCM block says what happens when it ends.
// A block that ends without repeating stops the voice dead, which is
// how a one-shot sound finishes: nothing has to key it off.
constexpr u8 BLOCK_LOOP_END = 1 << 0;
constexpr u8 BLOCK_LOOP_REPEAT = 1 << 1;
constexpr u8 BLOCK_LOOP_START = 1 << 2;

// The ADPCM filter: each sample is a four-bit difference plus a
// prediction made from the two before it, so a block cannot be decoded
// without the tail of the one in front. The five weightings are the
// hardware's, in sixty-fourths.
constexpr std::array<s32, 5> FILTER_OLD = {0, 60, 115, 98, 122};
constexpr std::array<s32, 5> FILTER_OLDER = {0, 0, -52, -55, -60};

// The pitch counter's fraction: twelve bits of it to a sample, so
// 0x1000 is the rate the sample data was recorded at. Software may ask
// for up to four samples a step and no more — a voice cannot be played
// faster than two octaves up.
constexpr u32 PITCH_BITS = 12;
constexpr u32 PITCH_UNIT = 1u << PITCH_BITS;
constexpr u32 PITCH_MAX = 0x3FFF;

constexpr s32 ENVELOPE_MAX = 0x7FFF;

// The rate an envelope moves at, as software gives it: a shift, which
// is the time and doubles with every count, and a step, which is the
// distance. Between them they cover a click and a fade lasting the
// better part of a minute.
struct Rate {
    u32 shift;
    s32 step;
    bool exponential;
};

// Attack and sustain are each given as a seven-bit rate holding both
// numbers at once: the shift above, the step below. Decay and release
// are shifts alone, since their step is fixed.
u32 rate_shift(u32 rate) { return (rate >> 2) & 0x1F; }
u32 rate_step(u32 rate) { return rate & 3; }

// The same rate as the two numbers the generator actually uses.
struct Slope {
    u32 wait;  // samples until the level next moves
    s32 step;  // how far it moves when it does
};

Slope slope_of(const Rate& rate, s32 level)
{
    u32 wait = 1;
    s32 step = rate.step;

    // Below a shift of eleven there is more than a whole step to make
    // each sample, and above it there is less than one — so the same
    // pair of numbers is read as a distance in one direction and as a
    // wait in the other.
    if (rate.shift > 11) {
        wait = 1u << (rate.shift - 11);
    } else {
        step <<= static_cast<s32>(11 - rate.shift);
    }

    // Exponential means the same fraction each time rather than the
    // same distance. Rising, it is approximated by slowing to a
    // quarter speed near the top; falling, the step shrinks with the
    // level, which is what makes a note die away rather than stop.
    if (rate.exponential) {
        if (step > 0 && level > 0x6000) {
            wait *= 4;
        }
        if (step < 0) {
            // Shifted rather than divided, and the difference matters:
            // a division rounds towards zero, so once the level is
            // small enough the step rounds away to nothing and the
            // note never finishes releasing. A shift rounds down, so
            // a falling envelope always falls by at least one.
            step = static_cast<s32>((s64{step} * level) >> 15);
        }
    }
    return {wait, step};
}

// A volume register: a fixed level, or a sweep that rises or falls on
// its own. Only the fixed form is modelled — a sweep is taken at full
// volume, so a fade in or out is heard as the sound simply being
// there. It is the loud way to be wrong, but the quiet one loses whole
// sounds that a game only ever fades up.
s32 volume_of(u16 value)
{
    if ((value & 0x8000) != 0) {
        return ENVELOPE_MAX;
    }
    // The fifteen bits are half the volume, so that the full range is
    // a signed sixteen-bit multiplier.
    return static_cast<s16>(static_cast<u16>(value << 1));
}

s16 clamp_sample(s32 value)
{
    return static_cast<s16>(std::clamp(value, -0x8000, 0x7FFF));
}

}  // namespace

void Spu::reset()
{
    registers = {};
    ram = {};
    voices = {};
    transfer_address = 0;
    key_on = 0;
    key_off = 0;
    ended = 0;
    irq_flag = false;
    cd_left = 0;
    cd_right = 0;
    output = {};
    produced = 0;
    taken = 0;
}

void Spu::visit_state(State& state)
{
    state(registers);
    state(ram);
    state(voices);
    state(transfer_address);
    state(key_on);
    state(key_off);
    state(ended);
    state(irq_flag);
    state(cd_left);
    state(cd_right);
}

u16 Spu::voice_register(u32 voice, u32 offset) const
{
    return registers[index_of(BASE + voice * VOICE_STRIDE + offset)];
}

u16 Spu::control() const { return registers[index_of(CONTROL)]; }

u16 Spu::status() const
{
    // The mode the SPU is in, which is simply the mode it was asked
    // for: there is nothing here that takes time to adopt one, and
    // software waits on these six bits before it will go on.
    u16 value = control() & STATUS_MODE_MASK;

    if (irq_flag) {
        value |= STATUS_IRQ_FLAG;
    }

    // Which way the SPU wants the DMA controller to move data, which
    // follows from the transfer mode and nothing else. The busy bit
    // above them stays clear: a transfer here finishes inside the
    // store that asked for it, so there is never a moment when
    // software could see one in progress.
    switch ((control() & CONTROL_TRANSFER_MODE) >> 4) {
    case TRANSFER_DMA_WRITE:
        value |= STATUS_TRANSFER_REQUEST | STATUS_DMA_WRITE_REQUEST;
        break;
    case TRANSFER_DMA_READ:
        value |= STATUS_TRANSFER_REQUEST | STATUS_DMA_READ_REQUEST;
        break;
    default:
        break;
    }
    return value;
}

bool Spu::interrupt_pending() const { return irq_flag; }

u32 Spu::active_voices() const
{
    u32 count = 0;
    for (const Voice& voice : voices) {
        if (voice.phase != Voice::Phase::Off) {
            count++;
        }
    }
    return count;
}

void Spu::transfer(u16 value)
{
    const u32 address = transfer_address % RAM_SIZE;
    ram[address] = static_cast<u8>(value);
    ram[address + 1] = static_cast<u8>(value >> 8);

    const u32 armed = u32{registers[index_of(IRQ_ADDRESS)]} * ADDRESS_UNIT;
    if ((control() & CONTROL_IRQ_ENABLE) != 0 && address == armed) {
        irq_flag = true;
    }

    transfer_address = (transfer_address + 2) % RAM_SIZE;
}

void Spu::write_dma(u32 word)
{
    transfer(static_cast<u16>(word));
    transfer(static_cast<u16>(word >> 16));
}

u32 Spu::read_dma()
{
    u32 word = 0;
    for (u32 i = 0; i < 2; i++) {
        const u32 address = transfer_address % RAM_SIZE;
        const u32 half = u32{ram[address]} | (u32{ram[address + 1]} << 8);
        word |= half << (i * 16);
        transfer_address = (transfer_address + 2) % RAM_SIZE;
    }
    return word;
}

void Spu::decode_block(u32 voice_index)
{
    Voice& voice = voices[voice_index];
    const u32 address = voice.address % RAM_SIZE;

    // A voice reading the block software armed is the other way the
    // SPU interrupts, and the one a game uses to be told where in a
    // long sample it has got to. Hardware checks each sample as it is
    // read, which is finer than this by a block.
    const u32 armed = u32{registers[index_of(IRQ_ADDRESS)]} * ADDRESS_UNIT;
    if ((control() & CONTROL_IRQ_ENABLE) != 0 && armed >= address &&
        armed < address + BLOCK_SIZE) {
        irq_flag = true;
    }

    // Sample memory wraps, and a block at the very end of it is read
    // half from each end rather than off the end.
    const auto byte_at = [&](u32 offset) {
        return ram[(address + offset) % RAM_SIZE];
    };

    const u8 header = byte_at(0);
    voice.flags = byte_at(1);

    // A block can say it is where the loop comes back to, instead of
    // the voice's repeat register being set by software.
    if ((voice.flags & BLOCK_LOOP_START) != 0) {
        voice.repeat_address = address;
        registers[index_of(BASE + voice_index * VOICE_STRIDE +
                           VOICE_REPEAT_ADDRESS)] =
            static_cast<u16>(address / ADDRESS_UNIT);
    }

    // A shift past twelve is not a finer step but a broken block, and
    // the hardware reads it as nine.
    u32 shift = header & 0x0F;
    if (shift > 12) {
        shift = 9;
    }
    const u32 filter = std::min<u32>((header >> 4) & 0x0F, 4);

    for (u32 i = 0; i < SAMPLES_PER_BLOCK; i++) {
        const u8 byte = byte_at(2 + i / 2);
        const u32 nibble = (i % 2 == 0) ? (byte & 0x0F) : (byte >> 4);

        // The difference is four bits at the top of a signed sixteen,
        // brought down by the block's shift.
        s32 sample = static_cast<s16>(nibble << 12) >> shift;
        sample += (voice.adpcm_old * FILTER_OLD[filter] +
                   voice.adpcm_older * FILTER_OLDER[filter] + 32) >>
            6;
        sample = std::clamp(sample, -0x8000, 0x7FFF);

        voice.block[i] = static_cast<s16>(sample);
        voice.adpcm_older = voice.adpcm_old;
        voice.adpcm_old = sample;
    }
}

void Spu::next_block(u32 voice_index)
{
    Voice& voice = voices[voice_index];

    // The flags on the block just finished say where to go next.
    if ((voice.flags & BLOCK_LOOP_END) != 0) {
        ended |= 1u << voice_index;
        voice.address = voice.repeat_address;
        if ((voice.flags & BLOCK_LOOP_REPEAT) == 0) {
            // A one-shot sound: the data itself says the voice is
            // finished, and it stops rather than releasing.
            voice.phase = Voice::Phase::Off;
            voice.level = 0;
        }
    } else {
        voice.address = (voice.address + BLOCK_SIZE) % RAM_SIZE;
    }

    if (voice.phase == Voice::Phase::Off) {
        return;  // nothing left to read; a key on starts it over
    }
    decode_block(voice_index);
    voice.index = 0;
}

void Spu::advance_voice(u32 voice_index)
{
    Voice& voice = voices[voice_index];
    voice.previous_sample = voice.sample;

    if (voice.index >= SAMPLES_PER_BLOCK) {
        next_block(voice_index);
        if (voice.phase == Voice::Phase::Off) {
            voice.sample = 0;
            return;
        }
    }

    voice.sample = voice.block[voice.index];
    voice.index++;
}

s32 Spu::voice_output(u32 voice_index)
{
    Voice& voice = voices[voice_index];

    // Between the last two samples, at wherever between them the pitch
    // has carried the voice. Straight lines rather than the hardware's
    // four-point curve, which costs a sample of delay and a little
    // brightness and saves a table of five hundred coefficients.
    const s32 span = voice.sample - voice.previous_sample;
    const s32 fraction = static_cast<s32>(voice.counter);
    const s32 sample =
        voice.previous_sample + ((span * fraction) >> PITCH_BITS);

    // Pitch modulation, where a voice's step is bent by the one before
    // it, is not modelled: every voice steps at the rate it was given.
    const u32 step = std::min<u32>(
        voice_register(voice_index, VOICE_SAMPLE_RATE), PITCH_MAX);
    voice.counter += step;
    while (voice.counter >= PITCH_UNIT) {
        voice.counter -= PITCH_UNIT;
        advance_voice(voice_index);
    }

    return sample;
}

void Spu::step_envelope(u32 voice_index)
{
    Voice& voice = voices[voice_index];
    if (voice.wait > 0) {
        voice.wait--;
        return;
    }

    const u16 lo = voice_register(voice_index, VOICE_ADSR_LO);
    const u16 hi = voice_register(voice_index, VOICE_ADSR_HI);

    // The sustain level is given in units of 0x800, one above what was
    // written, so the whole range is reachable.
    const s32 sustain_level =
        std::min<s32>((s32{lo & 0x0F} + 1) * 0x800, ENVELOPE_MAX);

    // Which phase the level says the voice is in, settled before any
    // step is taken: a phase can be over before it has moved at all —
    // an attack that reaches the top of a sound whose sustain level is
    // the top has nothing left to decay through — and stepping first
    // would put a dip in every note that has one.
    while (true) {
        if (voice.phase == Voice::Phase::Attack &&
            voice.level >= ENVELOPE_MAX) {
            voice.phase = Voice::Phase::Decay;
            continue;
        }
        if (voice.phase == Voice::Phase::Decay &&
            voice.level <= sustain_level) {
            voice.phase = Voice::Phase::Sustain;
            continue;
        }
        if (voice.phase == Voice::Phase::Release && voice.level <= 0) {
            voice.phase = Voice::Phase::Off;
        }
        break;
    }

    Rate rate = {};
    switch (voice.phase) {
    case Voice::Phase::Attack: {
        const u32 given = (lo >> 8) & 0x7F;
        rate = {rate_shift(given),
                7 - static_cast<s32>(rate_step(given)),
                (lo & 0x8000) != 0};
        break;
    }
    case Voice::Phase::Decay:
        // Always an exponential fall, at a rate given as a shift
        // alone: there is nothing for software to choose here.
        rate = {(lo >> 4) & 0x0Fu, -8, true};
        break;
    case Voice::Phase::Sustain: {
        const u32 given = (hi >> 6) & 0x7F;
        const s32 amount = static_cast<s32>(rate_step(given));
        const bool falling = (hi & (1 << 14)) != 0;
        s32 step = 7 - amount;
        if (falling) {
            step = -8 + amount;
        }
        rate = {rate_shift(given), step, (hi & 0x8000) != 0};
        break;
    }
    case Voice::Phase::Release:
        rate = {hi & 0x1Fu, -8, (hi & (1 << 5)) != 0};
        break;
    case Voice::Phase::Off:
        return;
    }

    const Slope slope = slope_of(rate, voice.level);
    voice.level = std::clamp(voice.level + slope.step, 0, ENVELOPE_MAX);
    voice.wait = slope.wait - 1;
}

void Spu::start_voice(u32 voice_index)
{
    Voice& voice = voices[voice_index];
    const u32 start =
        u32{voice_register(voice_index, VOICE_START_ADDRESS)} * ADDRESS_UNIT;

    voice = Voice{};
    voice.phase = Voice::Phase::Attack;
    voice.address = start % RAM_SIZE;

    // Keying on takes the repeat address with it, so a voice restarted
    // without its loop point being rewritten does not jump back into
    // the sound it was playing before.
    voice.repeat_address = voice.address;
    registers[index_of(BASE + voice_index * VOICE_STRIDE +
                       VOICE_REPEAT_ADDRESS)] =
        static_cast<u16>(voice.address / ADDRESS_UNIT);

    // The first sample is ready the moment the voice is keyed on, so
    // it starts on the next tick rather than one after that.
    decode_block(voice_index);
    voice.sample = voice.block[0];
    voice.index = 1;

    ended &= ~(1u << voice_index);
}

void Spu::stop_voice(u32 voice_index)
{
    Voice& voice = voices[voice_index];
    if (voice.phase == Voice::Phase::Off) {
        return;
    }
    voice.phase = Voice::Phase::Release;
    voice.wait = 0;
}

void Spu::push_output(s32 left, s32 right)
{
    output[produced % OUTPUT_FRAMES] = {clamp_sample(left),
                                        clamp_sample(right)};
    produced++;

    // Nothing here can stall the machine, so a host that stops
    // draining loses the oldest frames instead.
    if (produced - taken > OUTPUT_FRAMES) {
        taken = produced - OUTPUT_FRAMES;
    }
}

u32 Spu::take_output(Frame* frames, u32 count)
{
    const u32 available =
        static_cast<u32>(std::min<u64>(count, produced - taken));
    for (u32 i = 0; i < available; i++) {
        frames[i] = output[(taken + i) % OUTPUT_FRAMES];
    }
    taken += available;
    return available;
}

u32 Spu::output_ready() const { return static_cast<u32>(produced - taken); }

void Spu::set_cd_input(s16 left, s16 right)
{
    cd_left = left;
    cd_right = right;
}

bool Spu::tick()
{
    const bool was_pending = irq_flag;

    const u32 noise = u32{registers[index_of(NOISE_MODE)]} |
        (u32{registers[index_of(NOISE_MODE + 2)]} << 16);

    s32 left = 0;
    s32 right = 0;
    for (u32 i = 0; i < VOICE_COUNT; i++) {
        if (voices[i].phase == Voice::Phase::Off) {
            continue;
        }

        const s32 sample = voice_output(i);
        const s32 shaped = (sample * voices[i].level) >> 15;
        step_envelope(i);

        // A voice in noise mode plays the generator rather than its
        // sample data, and there is no generator here — so it is
        // decoded and thrown away, which keeps the end-of-sample flag
        // software waits on honest while the noise itself is missing.
        if ((noise & (1u << i)) != 0) {
            continue;
        }

        left +=
            (shaped * volume_of(voice_register(i, VOICE_VOLUME_LEFT))) >> 15;
        right +=
            (shaped * volume_of(voice_register(i, VOICE_VOLUME_RIGHT))) >> 15;
    }

    // Reverb would be mixed in here. It does not exist, so what the
    // voices made is all there is of the SPU's own output.
    const u16 mode = control();
    if ((mode & CONTROL_ENABLE) == 0 || (mode & CONTROL_UNMUTE) == 0) {
        left = 0;
        right = 0;
    }

    // The drive's sound joins after that, because neither the enable
    // nor the mute is its: they switch off the voices, and a console
    // playing a CD through a game that has switched its own sound
    // processor off still plays the CD. Its own bit and its own volume
    // are what stop it.
    if ((mode & CONTROL_CD_AUDIO) != 0) {
        // These two are not written the way the voices' volumes are:
        // there is no sweep behind them, so all sixteen bits are the
        // volume and unity is the top of them rather than half of it.
        const s32 cd_volume_left =
            static_cast<s16>(registers[index_of(CD_VOLUME_LEFT)]);
        const s32 cd_volume_right =
            static_cast<s16>(registers[index_of(CD_VOLUME_RIGHT)]);
        left += (cd_left * cd_volume_left) >> 15;
        right += (cd_right * cd_volume_right) >> 15;
    }

    // The sum of the voices is a sixteen-bit signal before the main
    // volume touches it, so two dozen loud voices clip here rather
    // than wrapping round to the opposite extreme.
    const s32 mixed_left = clamp_sample(left);
    const s32 mixed_right = clamp_sample(right);
    const s32 main_left = volume_of(registers[index_of(MAIN_VOLUME_LEFT)]);
    const s32 main_right = volume_of(registers[index_of(MAIN_VOLUME_RIGHT)]);
    push_output((mixed_left * main_left) >> 15,
                (mixed_right * main_right) >> 15);

    return irq_flag && !was_pending;
}

u16 Spu::read_register(u32 phys)
{
    if (phys < VOICE_END) {
        const u32 voice_index = (phys - BASE) / VOICE_STRIDE;
        switch ((phys - BASE) % VOICE_STRIDE) {
        case VOICE_ADSR_VOLUME:
            // Where the envelope has got to, which is how software
            // watches a sound die away without timing it itself.
            return static_cast<u16>(voices[voice_index].level);
        case VOICE_REPEAT_ADDRESS:
            return static_cast<u16>(voices[voice_index].repeat_address /
                                    ADDRESS_UNIT);
        default:
            break;
        }
        return registers[index_of(phys)];
    }

    switch (phys) {
    case STATUS:
        return status();
    case ENDX:
        return static_cast<u16>(ended);
    case ENDX + 2:
        return static_cast<u16>(ended >> 16);
    default:
        break;
    }

    // Everything else reads back what was written to it, which is what
    // the hardware does for the volumes and the reverb configuration.
    return registers[index_of(phys)];
}

void Spu::write_register(u32 phys, u16 value)
{
    registers[index_of(phys)] = value;

    if (phys < VOICE_END) {
        const u32 voice_index = (phys - BASE) / VOICE_STRIDE;
        switch ((phys - BASE) % VOICE_STRIDE) {
        case VOICE_ADSR_VOLUME:
            // Software can put the envelope where it likes, which is
            // how a sound is faded by hand rather than by the rates.
            voices[voice_index].level =
                std::clamp<s32>(static_cast<s16>(value), 0, ENVELOPE_MAX);
            break;
        case VOICE_REPEAT_ADDRESS:
            voices[voice_index].repeat_address =
                (u32{value} * ADDRESS_UNIT) % RAM_SIZE;
            break;
        default:
            break;
        }
        return;
    }

    switch (phys) {
    case TRANSFER_ADDRESS:
        transfer_address = u32{value} * ADDRESS_UNIT;
        break;

    case TRANSFER_FIFO:
        // Hardware queues these thirty-two deep and moves them when
        // the control register says to. Writing straight through is
        // the same thing to anything that cannot see the FIFO's depth,
        // and nothing can: there is no register that reports it.
        transfer(value);
        break;

    case CONTROL:
        // Turning the interrupt off is how software acknowledges one.
        if ((value & CONTROL_IRQ_ENABLE) == 0) {
            irq_flag = false;
        }
        break;

    case KEY_ON:
    case KEY_ON + 2: {
        const u32 shift = (phys == KEY_ON) ? 0 : 16;
        const u32 started = u32{value} << shift;
        key_on |= started;
        key_off &= ~started;
        for (u32 i = 0; i < VOICE_COUNT; i++) {
            if ((started & (1u << i)) != 0) {
                start_voice(i);
            }
        }
        break;
    }

    case KEY_OFF:
    case KEY_OFF + 2: {
        const u32 shift = (phys == KEY_OFF) ? 0 : 16;
        const u32 stopped = u32{value} << shift;
        key_off |= stopped;
        key_on &= ~stopped;
        for (u32 i = 0; i < VOICE_COUNT; i++) {
            if ((stopped & (1u << i)) != 0) {
                stop_voice(i);
            }
        }
        break;
    }

    default:
        break;
    }
}
