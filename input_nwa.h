#pragma once
#include <foobar2000/SDK/foobar2000.h>

class input_nwa {
public:
    typedef input_decoder_vrequired     interface_decoder_t;
    typedef input_info_reader_vrequired interface_info_reader_t;
    typedef input_info_writer_vrequired interface_info_writer_t;

    input_nwa();
    ~input_nwa();

    // life cycle methods
    void open(service_ptr_t<file> p_file, const char* p_path, t_input_open_reason p_reason, abort_callback& p_abort);
    void get_info(uint32_t p_subsong, file_info& p_info, abort_callback& p_abort);
    void decode_initialize(t_uint32 p_flags, uint32_t p_subsong, abort_callback& p_abort);
    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort);
    void decode_seek(double p_seconds, abort_callback& p_abort);
    bool decode_can_seek();
    bool decode_get_dynamic_info(file_info& p_out, double& p_title_changed);
    bool decode_get_dynamic_info_track(file_info& p_out, double& p_timestamp_delta);
    void decode_on_idle(abort_callback& p_abort);
    void set_logger(event_logger::ptr p_logger);
    foobar2000_io::t_filestats2 get_stats2(uint32_t p_flags, abort_callback& p_abort);
    uint32_t get_subsong(uint32_t p_index);
    uint32_t get_subsong_count();
    void retag_set_info(uint32_t p_subsong, const file_info& p_info, abort_callback& p_abort);
    void retag_commit(abort_callback& p_abort);
    static bool g_is_our_content_type(const char* p_content_type);
    static bool g_is_our_path(const char* p_path, const char* p_extension);
    static bool g_fallback_is_our_payload(const void* p_buffer, size_t p_bytes, foobar2000_io::t_filesize p_file_size) {
        return false;
    }

    static bool g_is_low_merit() { return false; }

    static GUID g_get_preferences_guid() { return pfc::guid_null; }

    static const char* g_get_name() { return "NWA Decoder"; }

    static GUID g_get_guid() {
        static const GUID guid = { 0xa5e4d120, 0x3c8b, 0x4c9d, { 0x9f, 0x1f, 0x7a, 0x6b, 0x5c, 0x4d, 0x3e, 0x2f } };
        return guid;
    }

private:
    // helper function to load blocks
    void load_block(t_uint32 p_block_index, abort_callback& p_abort);
    t_uint32 get_bits(t_uint32 p_count);
    t_int32 decode_sample(t_uint32 p_channel);

    service_ptr_t<file> m_file;
    pfc::string8 m_path;

    // basic info
    t_uint16 m_num_channels;
    t_uint16 m_bits_per_sample;
    t_uint32 m_sample_rate;
    t_int32  m_compression_level;
    t_uint32 m_total_samples;
    t_uint32 m_bytes_per_sample;

    // nwa spec info
    t_uint32 m_use_rle;
    t_uint32 m_num_blocks;
    t_uint32 m_samples_per_block;
    t_uint32 m_samples_in_last_block;

    //store offsets for all blocks
    pfc::array_t<t_uint32> m_block_offsets;

    // used for current decoding process 
    t_uint32 m_current_block_index;
    t_uint32 m_samples_decoded_in_block;
    // cache current block data
    pfc::array_t<t_uint8> m_current_block_data; 

    // ptr for bitstream reading
    t_size m_bit_ptr_byte;
    t_size m_bit_ptr_bit;

    // states for DPCM
    t_uint32 m_runlength;
    // record last sample for every channel
    pfc::array_t<t_int32> m_last_frame;

    // offset or uncompressed data start position (default 44)
    t_uint64 m_data_start_offset;

    // the current sample position
    t_uint64 m_current_sample_position;

    // normal cases lookup table
    t_uint32 m_bits_table[8];
    t_uint32 m_shift_table[8];
};
