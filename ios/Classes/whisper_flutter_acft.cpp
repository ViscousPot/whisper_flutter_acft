#include "whisper_flutter_acft.h"
#include "whisper.cpp/whisper.h"

#define DR_WAV_IMPLEMENTATION
#include "whisper.cpp/examples/dr_wav.h"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <cmath>
#include <map>
#include <mutex>
#include <iostream>
#include <stdio.h>
#include "json/json.hpp"

using json = nlohmann::json;

char *jsonToChar(json jsonData) noexcept
{
    std::string result = jsonData.dump();
    char *ch = new char[result.size() + 1];
    strcpy(ch, result.c_str());
    return ch;
}

struct whisper_params
{
    int32_t seed = -1; // RNG seed, not used currently
    int32_t n_threads = std::min(4, (int32_t)std::thread::hardware_concurrency());

    int32_t n_processors = 1;
    int32_t offset_t_ms = 0;
    int32_t offset_n = 0;
    int32_t duration_ms = 0;
    int32_t max_context = -1;
    int32_t max_len = 0;
    int32_t best_of = 5;
    int32_t beam_size = -1;

    float word_thold = 0.01f;
    float entropy_thold = 2.40f;
    float logprob_thold = -1.00f;

    bool verbose = false;
    bool print_special_tokens = false;
    bool speed_up = false;
    bool translate = false;
    bool diarize = false;
    bool no_fallback = false;
    bool output_txt = false;
    bool output_vtt = false;
    bool output_srt = false;
    bool output_wts = false;
    bool output_csv = false;
    bool print_special = false;
    bool print_colors = false;
    bool print_progress = false;
    bool no_timestamps = false;
    bool split_on_word = false;

    std::string language = "auto";
    std::string prompt;
    std::string initial_prompt;
    std::string model = "models/ggml-tiny.bin";
    std::string audio = "samples/jfk.wav";
    std::vector<std::string> fname_inp = {};
    std::vector<std::string> fname_outp = {};
};

struct whisper_print_user_data
{
    const whisper_params *params;

    const std::vector<std::vector<float>> *pcmf32s;
};

// --- Streaming session management ---

struct StreamSession {
    whisper_context* ctx;
    std::vector<float> audio_buffer;
    std::string committed_text;
    whisper_params params;
    bool processing = false;
};

static std::map<int, StreamSession*> g_sessions;
static int g_next_session_id = 1;
static std::mutex g_sessions_mutex;

static std::vector<unsigned char> base64_decode(const std::string &input) {
    static const unsigned char d[] = {
        0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 62,  0,  0,  0, 63,
       52, 53, 54, 55, 56, 57, 58, 59, 60, 61,  0,  0,  0,  0,  0,  0,
        0,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
       15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,  0,  0,  0,  0,  0,
        0, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
       41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51
    };
    std::vector<unsigned char> result;
    int val = 0, valb = -8;
    for (unsigned char c : input) {
        if (c == '=' || c >= 128) break;
        if (c < sizeof(d)) {
            val = (val << 6) + d[c];
            valb += 6;
            if (valb >= 0) {
                result.push_back((unsigned char)((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
    }
    return result;
}

json streamInit(json jsonBody) noexcept {
    json jsonResult;
    jsonResult["@type"] = "streamInit";

    std::string model = jsonBody.value("model", std::string(""));
    std::string language = jsonBody.value("language", std::string("auto"));
    int threads = jsonBody.value("threads", 4);
    std::string initial_prompt = jsonBody.value("initial_prompt", std::string(""));

    if (model.empty()) {
        jsonResult["@type"] = "error";
        jsonResult["message"] = "model path required for streamInit";
        return jsonResult;
    }

    struct whisper_context *ctx = whisper_init_from_file(model.c_str());
    if (!ctx) {
        jsonResult["@type"] = "error";
        jsonResult["message"] = "failed to initialize whisper model";
        return jsonResult;
    }

    StreamSession *session = new StreamSession();
    session->ctx = ctx;
    session->params.model = model;
    session->params.language = language;
    session->params.n_threads = threads;
    session->params.initial_prompt = initial_prompt;

    int session_id;
    {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        session_id = g_next_session_id++;
        g_sessions[session_id] = session;
    }

    jsonResult["session_id"] = session_id;
    return jsonResult;
}

json streamProcess(json jsonBody) noexcept {
    json jsonResult;
    jsonResult["@type"] = "streamProcess";

    int session_id = jsonBody.value("session_id", 0);
    std::string audio_data = jsonBody.value("audio_data", std::string(""));

    StreamSession *session = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        auto it = g_sessions.find(session_id);
        if (it == g_sessions.end()) {
            jsonResult["@type"] = "error";
            jsonResult["message"] = "session not found";
            return jsonResult;
        }
        session = it->second;
    }

    // Decode base64 audio data and append to buffer
    if (!audio_data.empty()) {
        std::vector<unsigned char> decoded = base64_decode(audio_data);
        const float *float_data = reinterpret_cast<const float*>(decoded.data());
        size_t num_floats = decoded.size() / sizeof(float);
        session->audio_buffer.insert(session->audio_buffer.end(), float_data, float_data + num_floats);
    }

    const int min_samples = 16160;
    if ((int)session->audio_buffer.size() < min_samples) {
        jsonResult["text"] = "";
        jsonResult["committed_text"] = session->committed_text;
        return jsonResult;
    }

    // Make a copy for processing (buffer may be padded)
    std::vector<float> buffer_copy = session->audio_buffer;
    const int original_samples = (int)buffer_copy.size();
    if ((int)buffer_copy.size() < min_samples) {
        buffer_copy.resize(min_samples, 0.0f);
    }

    // Set up whisper params
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_realtime = false;
    wparams.print_progress = false;
    wparams.print_timestamps = true;
    wparams.translate = false;
    wparams.language = session->params.language.c_str();
    wparams.initial_prompt = session->params.initial_prompt.c_str();
    wparams.n_threads = session->params.n_threads;
    wparams.no_context = true;
    wparams.audio_ctx = std::min(1500, (int)ceil((double)original_samples / 320.0) + 8);

    if (whisper_full(session->ctx, wparams, buffer_copy.data(), buffer_copy.size()) != 0) {
        jsonResult["@type"] = "error";
        jsonResult["message"] = "failed to process audio";
        return jsonResult;
    }

    // Extract text from all segments
    std::string text_result = "";
    const int n_segments = whisper_full_n_segments(session->ctx);
    for (int i = 0; i < n_segments; ++i) {
        text_result += whisper_full_get_segment_text(session->ctx, i);
    }

    // Window management: if buffer > 27s (432000 samples at 16kHz),
    // commit text and keep last 3s (48000 samples) as overlap
    const int max_window_samples = 432000;
    const int overlap_samples = 48000;
    if ((int)session->audio_buffer.size() > max_window_samples) {
        session->committed_text += text_result;
        text_result = "";
        size_t keep_from = session->audio_buffer.size() - overlap_samples;
        std::vector<float> overlap(session->audio_buffer.begin() + keep_from, session->audio_buffer.end());
        session->audio_buffer = overlap;
    }

    jsonResult["text"] = text_result;
    jsonResult["committed_text"] = session->committed_text;
    return jsonResult;
}

json streamFinalize(json jsonBody) noexcept {
    json jsonResult;
    jsonResult["@type"] = "streamFinalize";

    int session_id = jsonBody.value("session_id", 0);

    StreamSession *session = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        auto it = g_sessions.find(session_id);
        if (it == g_sessions.end()) {
            jsonResult["@type"] = "error";
            jsonResult["message"] = "session not found";
            return jsonResult;
        }
        session = it->second;
    }

    std::string final_text = session->committed_text;

    // Process any remaining audio in buffer
    if (!session->audio_buffer.empty()) {
        std::vector<float> buffer_copy = session->audio_buffer;
        const int original_samples = (int)buffer_copy.size();
        const int min_samples = 16160;
        if ((int)buffer_copy.size() < min_samples) {
            buffer_copy.resize(min_samples, 0.0f);
        }

        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.print_realtime = false;
        wparams.print_progress = false;
        wparams.print_timestamps = true;
        wparams.translate = false;
        wparams.language = session->params.language.c_str();
        wparams.initial_prompt = session->params.initial_prompt.c_str();
        wparams.n_threads = session->params.n_threads;
        wparams.no_context = true;
        wparams.audio_ctx = std::min(1500, (int)ceil((double)original_samples / 320.0) + 8);

        if (whisper_full(session->ctx, wparams, buffer_copy.data(), buffer_copy.size()) == 0) {
            const int n_segments = whisper_full_n_segments(session->ctx);
            for (int i = 0; i < n_segments; ++i) {
                final_text += whisper_full_get_segment_text(session->ctx, i);
            }
        }
    }

    // Cleanup
    whisper_free(session->ctx);
    {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        g_sessions.erase(session_id);
    }
    delete session;

    jsonResult["text"] = final_text;
    return jsonResult;
}

json streamCancel(json jsonBody) noexcept {
    json jsonResult;
    jsonResult["@type"] = "streamCancel";

    int session_id = jsonBody.value("session_id", 0);

    StreamSession *session = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        auto it = g_sessions.find(session_id);
        if (it == g_sessions.end()) {
            jsonResult["success"] = true;
            return jsonResult;
        }
        session = it->second;
        g_sessions.erase(it);
    }

    whisper_free(session->ctx);
    delete session;

    jsonResult["success"] = true;
    return jsonResult;
}

json transcribe(json jsonBody) noexcept
{
    whisper_params params;

    params.n_threads = jsonBody["threads"];
    params.verbose = jsonBody["is_verbose"];
    params.translate = jsonBody["is_translate"];
    params.language = jsonBody["language"];
    params.initial_prompt = jsonBody["initial_prompt"];
    params.print_special_tokens = jsonBody["is_special_tokens"];
    params.no_timestamps = jsonBody["is_no_timestamps"];
    params.model = jsonBody["model"];
    params.audio = jsonBody["audio"];
    params.split_on_word = jsonBody["split_on_word"];
    json jsonResult;
    jsonResult["@type"] = "transcribe";

    if (params.language != "" && params.language != "auto" && whisper_lang_id(params.language.c_str()) == -1)
    {
        jsonResult["@type"] = "error";
        jsonResult["message"] = "error: unknown language = " + params.language;
        return jsonResult;
    }

    if (params.seed < 0)
    {
        params.seed = time(NULL);
    }

    // whisper init
    struct whisper_context *ctx = whisper_init_from_file(params.model.c_str());
    std::string text_result = "";
    const auto fname_inp = params.audio;
    // WAV input
    std::vector<float> pcmf32;
    {
        drwav wav;
        if (!drwav_init_file(&wav, fname_inp.c_str(), NULL))
        {
            jsonResult["@type"] = "error";
            jsonResult["message"] = " failed to open WAV file ";
            return jsonResult;
        }

        if (wav.channels != 1 && wav.channels != 2)
        {
            jsonResult["@type"] = "error";
            jsonResult["message"] = "must be mono or stereo";
            return jsonResult;
        }

        if (wav.sampleRate != WHISPER_SAMPLE_RATE)
        {
            jsonResult["@type"] = "error";
            jsonResult["message"] = "WAV file  must be 16 kHz";
            return jsonResult;
        }

        if (wav.bitsPerSample != 16)
        {
            jsonResult["@type"] = "error";
            jsonResult["message"] = "WAV file  must be 16 bit";
            return jsonResult;
        }

        int n = wav.totalPCMFrameCount;

        std::vector<int16_t> pcm16;
        pcm16.resize(n * wav.channels);
        drwav_read_pcm_frames_s16(&wav, n, pcm16.data());
        drwav_uninit(&wav);

        // convert to mono, float
        pcmf32.resize(n);
        if (wav.channels == 1)
        {
            for (int i = 0; i < n; i++)
            {
                pcmf32[i] = float(pcm16[i]) / 32768.0f;
            }
        }
        else
        {
            for (int i = 0; i < n; i++)
            {
                pcmf32[i] = float(pcm16[2 * i] + pcm16[2 * i + 1]) / 65536.0f;
            }
        }
    }

    // Save original sample count for audio_ctx calculation (before any padding)
    const int original_samples = (int)pcmf32.size();

    // Pad audio to bypass whisper.cpp's 1-second minimum guards
    // whisper.cpp requires seek_end > seek + 100, where seek_end = n_samples/160
    // So we need n_samples/160 > 100, i.e. n_samples > 16000
    const int min_samples = 16160; // 101 mel frames * 160 hop_length
    if ((int)pcmf32.size() < min_samples) {
        pcmf32.resize(min_samples, 0.0f);
    }

    {
        if (params.language == "" && params.language == "auto")
        {
            params.language = "auto";
            params.translate = false;
        }
    }
    // run the inference
    {
        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

        wparams.print_realtime = false;
        wparams.print_progress = false;
        wparams.print_timestamps = !params.no_timestamps;
        // wparams.print_special_tokens = params.print_special_tokens;
        wparams.translate = params.translate;
        wparams.language = params.language.c_str();
        wparams.initial_prompt = params.initial_prompt.c_str();
        wparams.n_threads = params.n_threads;
        wparams.split_on_word = params.split_on_word;

        if (params.split_on_word) {
            wparams.max_len = 1;
            wparams.token_timestamps = true;
        }

        // +8 matches upstream ACFT training (ADD_AUDIO_CTX = 8)
        // Use original_samples (not padded size) so padding doesn't inflate audio_ctx
        wparams.audio_ctx = std::min(1500, (int)ceil((double)original_samples / 320.0) + 8);

        if (whisper_full(ctx, wparams, pcmf32.data(), pcmf32.size()) != 0)
        {
            jsonResult["@type"] = "error";
            jsonResult["message"] = "failed to process audio";
            return jsonResult;
        }

        

        // print result;
        if (!wparams.print_realtime)
        {

            const int n_segments = whisper_full_n_segments(ctx);

            std::vector<json> segmentsJson = {};

            for (int i = 0; i < n_segments; ++i)
            {
                const char *text = whisper_full_get_segment_text(ctx, i);

                std::string str(text);
                text_result += str;
                if (params.no_timestamps)
                {
                    // printf("%s", text);
                    // fflush(stdout);
                } else {
                    json jsonSegment;
                    const int64_t t0 = whisper_full_get_segment_t0(ctx, i);
                    const int64_t t1 = whisper_full_get_segment_t1(ctx, i);

                    // printf("[%s --> %s]  %s\n", to_timestamp(t0).c_str(), to_timestamp(t1).c_str(), text);

                    jsonSegment["from_ts"] = t0;
                    jsonSegment["to_ts"] = t1;
                    jsonSegment["text"] = text;

                    segmentsJson.push_back(jsonSegment);
                }
            }

            if (!params.no_timestamps) {
                jsonResult["segments"] = segmentsJson;
            }
        }
    }
    jsonResult["text"] = text_result;
    
    whisper_free(ctx);
    return jsonResult;
}
extern "C"
{
    FUNCTION_ATTRIBUTE
    char *request(char *body)
    {
        try
        {
            json jsonBody = json::parse(body);
            json jsonResult;

            if (jsonBody["@type"] == "getTextFromWavFile")
            {
                try
                {
                    return jsonToChar(transcribe(jsonBody));
                }
                catch (const std::exception &e)
                {
                    jsonResult["@type"] = "error";
                    jsonResult["message"] = e.what();
                    return jsonToChar(jsonResult);
                }
            }
            if (jsonBody["@type"] == "getVersion")
            {
                jsonResult["@type"] = "version";
                jsonResult["message"] = "lib version: v1.0.1";
                return jsonToChar(jsonResult);
            }
            if (jsonBody["@type"] == "streamInit")
            {
                try
                {
                    return jsonToChar(streamInit(jsonBody));
                }
                catch (const std::exception &e)
                {
                    jsonResult["@type"] = "error";
                    jsonResult["message"] = e.what();
                    return jsonToChar(jsonResult);
                }
            }
            if (jsonBody["@type"] == "streamProcess")
            {
                try
                {
                    return jsonToChar(streamProcess(jsonBody));
                }
                catch (const std::exception &e)
                {
                    jsonResult["@type"] = "error";
                    jsonResult["message"] = e.what();
                    return jsonToChar(jsonResult);
                }
            }
            if (jsonBody["@type"] == "streamFinalize")
            {
                try
                {
                    return jsonToChar(streamFinalize(jsonBody));
                }
                catch (const std::exception &e)
                {
                    jsonResult["@type"] = "error";
                    jsonResult["message"] = e.what();
                    return jsonToChar(jsonResult);
                }
            }
            if (jsonBody["@type"] == "streamCancel")
            {
                try
                {
                    return jsonToChar(streamCancel(jsonBody));
                }
                catch (const std::exception &e)
                {
                    jsonResult["@type"] = "error";
                    jsonResult["message"] = e.what();
                    return jsonToChar(jsonResult);
                }
            }

            jsonResult["@type"] = "error";
            jsonResult["message"] = "method not found";
            return jsonToChar(jsonResult);
        }
        catch (const std::exception &e)
        {
            json jsonResult;
            jsonResult["@type"] = "error";
            jsonResult["message"] = e.what();
            return jsonToChar(jsonResult);
        }
    }
}