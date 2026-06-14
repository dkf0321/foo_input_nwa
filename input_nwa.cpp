#include "input_nwa.h"

input_nwa::input_nwa() :
    m_num_channels(0), m_bits_per_sample(0), m_sample_rate(0),
    m_compression_level(0), m_total_samples(0), m_bytes_per_sample(0),
    m_use_rle(0), m_num_blocks(0), m_samples_per_block(0), m_samples_in_last_block(0),
    m_current_block_index(0), m_samples_decoded_in_block(0),
    m_bit_ptr_byte(0), m_bit_ptr_bit(0), m_runlength(0),
    m_data_start_offset(44), m_current_sample_position(0) {
}

input_nwa::~input_nwa() {}

// open file and parse the header
void input_nwa::open(service_ptr_t<file> p_file, const char* p_path, t_input_open_reason p_reason, abort_callback& p_abort) {
    m_file = p_file;
    m_path = p_path;

    if (m_file.is_empty()) {
        foobar2000_io::filesystem::g_open(m_file, p_path, foobar2000_io::filesystem::open_mode_read, p_abort);
    }

    // header contains 44 bytes
    t_uint8 header[44];
    m_file->read_object(header, 44, p_abort);

    // parse little-endian header data
    // Sample: A single audio reading on a single channel
    // Frame: A single sample from all channels
    // Block: A compressed chunk of data in the file

    // The number of channels
    m_num_channels = *reinterpret_cast<t_uint16*>(&header[0]);
    // The number of bits in an audio sample (after decompression)
    m_bits_per_sample = *reinterpret_cast<t_uint16*>(&header[2]);
    // The number of frames per second
    m_sample_rate = *reinterpret_cast<t_uint32*>(&header[4]);
    // The compression level (-1 means uncompressed)
    m_compression_level = *reinterpret_cast<t_int32*>(&header[8]);
    // Flag (0 or 1) indicating if run-length encoding is used
    m_use_rle = *reinterpret_cast<t_uint32*>(&header[12]);
    // The number of blocks in a compressed file
    m_num_blocks = *reinterpret_cast<t_uint32*>(&header[16]);

    // header[20] - header[27] is not parsed

    // Number of samples in the file
    m_total_samples = *reinterpret_cast<t_uint32*>(&header[28]);
    // Number of samples in blocks except for the last one
    m_samples_per_block = *reinterpret_cast<t_uint32*>(&header[32]);
    // Number of samples in the last block
    m_samples_in_last_block = *reinterpret_cast<t_uint32*>(&header[36]);

    // header[41] - header[43] is unknown

    // > 16-bit audio is unsupported
    if (m_bits_per_sample != 8 && m_bits_per_sample != 16) {
        throw exception_io_data("NWA decoder: Unsupported bit depth. Only supports 8-bit or 16-bit audio.");
    }
    m_bytes_per_sample = m_bits_per_sample / 8;

    // if compressed
    if (m_compression_level != -1) {
        // precompute bits ans shift lookup table
        // exp == 1 ~ 7
        for (t_uint32 exp = 1; exp <= 7; ++exp) {
            // exp < 7
            if (exp < 7) {
                // comp lvl < 3
                if (m_compression_level < 3) {
                    m_bits_table[exp] = 4 - m_compression_level;
                    m_shift_table[exp] = 2 + exp + m_compression_level;
                }
                // comp lvl >= 3
                else {
                    m_bits_table[exp] = 2 + m_compression_level;
                    m_shift_table[exp] = 1 + exp;
                }
            }
            // exp == 7
            else {
                // comp lvl < 3
                if (m_compression_level < 3) {
                    m_bits_table[exp] = 7 - m_compression_level;
                    m_shift_table[exp] = 9 + m_compression_level;
                }
                // comp lvl >= 3
                else {
                    m_bits_table[exp] = 7;
                    m_shift_table[exp] = 9;
                }
            }
        }

        // read the offset value
        m_block_offsets.set_size(m_num_blocks);
        m_file->read_object(m_block_offsets.get_ptr(), m_num_blocks * 4, p_abort);
    }
}

void input_nwa::get_info(uint32_t p_subsong, file_info& p_info, abort_callback& p_abort) {
    // duration = total samples / channel num / sample rate
    double duration = 0.0;
    if (m_num_channels > 0 && m_sample_rate > 0) {
        duration = static_cast<double>(m_total_samples) / m_num_channels / m_sample_rate;
    }
    p_info.set_length(duration);

    // set general info
    p_info.info_set_int("channels", m_num_channels);
    p_info.info_set_int("bitspersample", m_bits_per_sample);
    p_info.info_set_int("samplerate", m_sample_rate);
    if (m_compression_level == -1) {
        p_info.info_set("codec", "NWA Audio (Uncompressed)");
    }
    else {
        p_info.info_set("codec", "NWA Audio (Compressed)");
    }

    // set other info
    p_info.info_set_int("block num", m_num_blocks);
    p_info.info_set_int("nwa compression level", m_compression_level);
    p_info.info_set_int("samples per block", m_samples_per_block);
    p_info.info_set_int("samples last block", m_samples_in_last_block);
    p_info.info_set_int("total samples", m_total_samples);
    p_info.info_set_int("use rle", m_use_rle);

    // set bitrate
    if (duration > 0.0) {
        t_uint64 file_size = m_file->get_size(p_abort);
        // bitrate (kbps) = (bytes * 8) / (second * 1000)
        t_int64 bitrate_kbps = static_cast<t_int64>((file_size * 8) / (duration * 1000));
        p_info.info_set_bitrate(bitrate_kbps);
    }
}

// initialize the decoder
void input_nwa::decode_initialize(t_uint32 p_flags, uint32_t p_subsong, abort_callback& p_abort) {
    m_current_sample_position = 0;

    // uncompressed format, start at 44
    if (m_compression_level == -1) {
        m_file->seek(m_data_start_offset, p_abort);
    }
    // compressed
    else {
        // load the first block by default
        load_block(0, p_abort);
    }
}

// read the compressed data of the specified block into memory and parse the seed status 
void input_nwa::load_block(t_uint32 p_block_index, abort_callback& p_abort) {
    m_current_block_index = p_block_index;
    m_samples_decoded_in_block = 0;
    m_runlength = 0;

    t_uint64 offset = m_block_offsets[p_block_index];
    t_uint64 next_offset = 0;
    if (p_block_index < m_num_blocks - 1) {
        next_offset = m_block_offsets[p_block_index + 1];
    }
    // last block
    else {
        next_offset = m_file->get_size(p_abort);
    }
    t_size block_size = static_cast<t_size>(next_offset - offset);

    m_current_block_data.set_size(block_size);
    m_file->seek(offset, p_abort);
    m_file->read_object(m_current_block_data.get_ptr(), block_size, p_abort);

    m_bit_ptr_byte = 0;
    m_bit_ptr_bit = 0;

    // parse the seed
    m_last_frame.set_size(m_num_channels);
    for (t_uint32 i = 0; i < m_num_channels; ++i) {
        t_int32 seed = 0;
        if (m_bytes_per_sample == 1) {
            seed = m_current_block_data[m_bit_ptr_byte];
            m_bit_ptr_byte += 1;
        }
        else if (m_bytes_per_sample == 2) {
            seed = *reinterpret_cast<t_int16*>(&m_current_block_data[m_bit_ptr_byte]);
            m_bit_ptr_byte += 2;
        }
        m_last_frame[i] = seed;
    }
}

// read bits (change to MSB)
t_uint32 input_nwa::get_bits(t_uint32 p_count) {
    if (p_count == 0) return 0;

    t_uint32 mask = (1 << p_count) - 1;

    t_uint32 val = m_current_block_data[m_bit_ptr_byte];

    // taken starting with the least significant bits of the next byte
    if (m_bit_ptr_bit + p_count > 8) {
        
        // bit ptr at last byte and bit needed out of current byte (rare)
        if (m_bit_ptr_byte + 1 >= m_current_block_data.get_size()) {
            throw exception_io_data("NWA decoder: Unexpected end of block bitstream. The file may be corrupted.");
        }

        val |= (m_current_block_data[m_bit_ptr_byte + 1] << 8);
    }

    val = (val >> m_bit_ptr_bit) & mask;

    m_bit_ptr_bit += p_count;
    m_bit_ptr_byte += m_bit_ptr_bit / 8;
    m_bit_ptr_bit %= 8;

    return val;
}

// decode one channel sample
t_int32 input_nwa::decode_sample(t_uint32 p_channel) {
    t_int32 mantissa = 0;
    t_int32 sign = 0;

    // if counter != 0, it means we are in RLE repeating, skip reading stream
    if (m_runlength != 0) {
        m_runlength--;
    }
    else {
        t_uint32 exp = get_bits(3);
        // exception A: exp == 0
        if (exp == 0) {
            // A-1: run-length encoding is used, the next bit(s) define the number of repeats
            if (m_use_rle == 1) {
                // A-1-1: First, read one bit. If it is 0, repeat once
                m_runlength = get_bits(1);
                // A-1-2: otherwise, read two more bits 
                if (m_runlength == 1) {
                    // A-1-2-1: If those bits are not binary 11, they signify the number of repeats
                    m_runlength = get_bits(2);
                    // A-1-2-2: Otherwise, read another eight bits to get the number of repeats
                    if (m_runlength == 3) {
                        m_runlength = get_bits(8);
                    }
                }
            }

            // A-2: run-length encoding is not used, the previous sample is repeated once
            else {
                // m_runlength keeps 0
                // mantissa keeps 0 (repeat previous once)
            }
        }

        // exp != 0
        else {
            // normal cases, use lookup table
            if (exp != 7 || get_bits(1) != 1) {
                t_uint32 bits = m_bits_table[exp];
                t_uint32 shift = m_shift_table[exp];

                mantissa = get_bits(bits) << shift;
                sign = get_bits(1);
            }
            // exception B: exp == 7 and bit1 == 1, the previous sample is repeated
            else {
                // m_runlength keeps 0
                // mantissa keeps 0 (repeat previous once)
            }
        }
    }

    if (sign == 1) {
        mantissa = -mantissa;
    }

    m_last_frame[p_channel] += mantissa;

    // clip to corresponding bit-depth
    if (m_bytes_per_sample == 1) {
        m_last_frame[p_channel] &= 0xFF;
    }
    else {
        m_last_frame[p_channel] &= 0xFFFF;
    }

    return m_last_frame[p_channel];
}

// core loop for streaming decode
bool input_nwa::decode_run(audio_chunk& p_chunk, abort_callback& p_abort) {
    if (m_current_sample_position >= m_total_samples) {
        return false;
    }

    const t_size frames_to_read = 1024;
    t_size samples_to_read = frames_to_read * m_num_channels;

    if (m_current_sample_position + samples_to_read > m_total_samples) {
        samples_to_read = m_total_samples - m_current_sample_position;
    }

    if (samples_to_read == 0) return false;

    pfc::array_t<t_uint8> buffer;
    buffer.set_size(samples_to_read * m_bytes_per_sample);
    t_uint8* out_ptr = buffer.get_ptr();

    // A: uncompressed
    if (m_compression_level == -1) {
        t_size bytes_to_read = samples_to_read * m_bytes_per_sample;
        t_size bytes_read = m_file->read(buffer.get_ptr(), bytes_to_read, p_abort);
        if (bytes_read == 0) return false;
        samples_to_read = bytes_read / m_bytes_per_sample;
    }
    // B: compressed
    else {
        t_uint32 target_samples_per_block = (m_current_block_index == m_num_blocks - 1) ? m_samples_in_last_block : m_samples_per_block;

        for (t_size s = 0; s < samples_to_read; ) {
            // if the current block is all parsed, load the next block
            if (m_samples_decoded_in_block >= target_samples_per_block) {
                // reach EOF, truncate samples to read
                if (m_current_block_index >= m_num_blocks - 1) {
                    samples_to_read = s;
                    break;
                }
                load_block(m_current_block_index + 1, p_abort);
                target_samples_per_block = (m_current_block_index == m_num_blocks - 1) ? m_samples_in_last_block : m_samples_per_block;
            }

            // decode a single frame channel by channel 
            for (t_uint32 ch = 0; ch < m_num_channels; ++ch) {
                t_int32 sample_val = decode_sample(ch);
                m_samples_decoded_in_block++;

                if (m_bytes_per_sample == 1) {
                    *out_ptr++ = static_cast<t_uint8>(sample_val);
                }
                else {
                    *reinterpret_cast<t_int16*>(out_ptr) = static_cast<t_int16>(sample_val);
                    out_ptr += 2;
                }
            }
            s += m_num_channels;
        }
    }

    if (samples_to_read == 0) return false;

    p_chunk.set_data_fixedpoint(
        buffer.get_ptr(),
        samples_to_read * m_bytes_per_sample,
        m_sample_rate,
        m_num_channels,
        m_bits_per_sample,
        audio_chunk::g_guess_channel_config(m_num_channels)
    );

    m_current_sample_position += samples_to_read;
    return true;
}

// low latency seeking
void input_nwa::decode_seek(double p_seconds, abort_callback& p_abort) {
    t_uint64 target_frame = static_cast<t_uint64>(p_seconds * m_sample_rate);
    t_uint64 target_sample = target_frame * m_num_channels;

    if (target_sample > m_total_samples) target_sample = m_total_samples;

    // seeking for uncompressed audio
    if (m_compression_level == -1) {
        t_uint64 byte_offset = m_data_start_offset + (target_sample * m_bytes_per_sample);
        m_file->seek(byte_offset, p_abort);
        m_current_sample_position = target_sample;
        return;
    }

    // seeking for compressed audio
    t_uint32 target_block = 0;
    t_uint64 sample_accumulator = 0;

    // 1. use offset data to locate target block
    for (t_uint32 i = 0; i < m_num_blocks; ++i) {
        t_uint32 block_samples = (i == m_num_blocks - 1) ? m_samples_in_last_block : m_samples_per_block;
        if (target_sample >= sample_accumulator && target_sample < sample_accumulator + block_samples) {
            target_block = i;
            break;
        }
        sample_accumulator += block_samples;
    }
    if (target_sample >= m_total_samples) {
        target_block = m_num_blocks - 1;
        target_sample = m_total_samples;
    }

    // 2. load target block and seed
    load_block(target_block, p_abort);

    // 3. parse some data in this block to calibrate DPCM
    t_uint64 samples_to_skip = target_sample - sample_accumulator;
    t_uint64 frames_to_skip = samples_to_skip / m_num_channels;

    for (t_uint64 f = 0; f < frames_to_skip; ++f) {
        for (t_uint32 ch = 0; ch < m_num_channels; ++ch) {
            decode_sample(ch);
            m_samples_decoded_in_block++;
        }
    }

    m_current_sample_position = sample_accumulator + (frames_to_skip * m_num_channels);
}

bool input_nwa::decode_can_seek() { return true; }
bool input_nwa::decode_get_dynamic_info(file_info& p_out, double& p_title_changed) { return false; }
bool input_nwa::decode_get_dynamic_info_track(file_info& p_out, double& p_timestamp_delta) { return false; }
void input_nwa::decode_on_idle(abort_callback& p_abort) {}

// file extension
bool input_nwa::g_is_our_content_type(const char* p_content_type) { return false; }
bool input_nwa::g_is_our_path(const char* p_path, const char* p_extension) {
    return stricmp_utf8(p_extension, "nwa") == 0;
}

// write tags is not supported
void input_nwa::retag_set_info(uint32_t p_subsong, const file_info& p_info, abort_callback& p_abort) {
    throw pfc::exception_not_implemented();
}
void input_nwa::retag_commit(abort_callback& p_abort) {
    throw pfc::exception_not_implemented();
}

// write log
void input_nwa::set_logger(event_logger::ptr p_logger) {
    // pass
}

// read file stat
foobar2000_io::t_filestats2 input_nwa::get_stats2(uint32_t p_flags, abort_callback& p_abort) {
    return foobar2000_io::filesystem::g_get_stats2(m_path, p_flags, p_abort);
}

// no subsong for nwa
uint32_t input_nwa::get_subsong(uint32_t p_index) { return p_index; }
uint32_t input_nwa::get_subsong_count() { return 1; }
