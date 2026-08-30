#include <thread>
#include <chrono>
#include <functional> // Requis pour std::hash
#include <unordered_map>
#include <unordered_set>
#include <stdint.h>

#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#include "./dr_libs/dr_wav.h"
#include "./dr_libs/dr_flac.h"
#include "./dr_libs/dr_mp3.h"

#include <vector>
#include <deque>

#include "miniaudio.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"

#include  "imgui_impl_sdlrenderer3.h"

#include "sse2_fft.h"

#include "kiss_fft.h"

#include "imfilebrowser.h"


extern "C" {
    #include "filters.h"
}

#include <SDL3/SDL.h>
#include <vector>
#include <mutex>
#include <cmath>
#include <iostream>

// Configuration Audio / Visuelle

 int BUFFER_SIZE = 8192;       // Taille du tampon circulaire
 // Nombre d'échantillons visibles à l'écran

int MIN_LAG = 22;
int MAX_LAG = 882;
int DISPLAY_SAMPLES = BUFFER_SIZE/2 - MAX_LAG;

constexpr  uint32_t WINDOW_WIDTH = 800;
constexpr  uint32_t WINDOW_HEIGHT = 600;

std::vector<float> samples;

alignas(64) std::atomic<bool> playback = false; 
alignas(64) std::atomic<bool> stop_playback_requested = false;
alignas(64) std::atomic<size_t> samples_cursor = 0, next_cursor = 0;
alignas(64) std::atomic<float> gain_db = 0.f;
alignas(64) std::atomic<float> current_playing_time = 0.;
alignas(64) std::atomic<float> gain = std::pow(10.f,gain_db/20.f);

alignas(64) std::atomic<uint> channels = 2; 
alignas(64) std::atomic<uint> sample_rate = 44100;
alignas(64) std::atomic<bool> file_loaded = false;
alignas(64) std::atomic<bool> file_loading = false;
alignas(64) std::atomic<bool> error_file_loading = false;
alignas(64) std::atomic<bool> close_dialog = false;

size_t last_audio_cursor_stream = 0;
alignas(64) std::atomic<size_t> audio_cursor_stream = 0;

std::string error_file_loading_msg;


std::vector<float> buffer(BUFFER_SIZE);  

struct filter_combo_item_t
{
    const char* label;
    bool is_selected;
};

struct reverb_filter_t
{
     sf_reverb_state_st rv;

     inline void set_sample_rate(int sample_rate)
     {
        sf_presetreverb(&rv, sample_rate, SF_REVERB_PRESET_DEFAULT);
     }
     sf_sample_st sample_in;
     sf_sample_st sample_out;
     inline void process(float *sample_L, float *sample_R)
     {
        sample_in = sf_sample_st{ *sample_L,*sample_R};
        sf_reverb_process(&rv, 1, &sample_in, &sample_out);
        *sample_L = sample_out.L;
        *sample_R = sample_out.R;
     }
};

struct compressor_filter_t
{
     sf_compressor_state_st compressor_state;

     inline void set_sample_rate(int sample_rate)
     {
        sf_defaultcomp(&compressor_state, sample_rate);
        
     }
     sf_sample_st sample_in[32];
     sf_sample_st sample_out[32];
     

     inline void process(float *sample_L, float *sample_R)
     {
        
        sample_in[0] = sf_sample_st{ *sample_L,*sample_R};
        sf_compressor_process(&compressor_state, 1, sample_in, sample_out);
        
        *sample_L = sample_out[0].L;
        *sample_R = sample_out[0].R;

     }
};

struct  flanger_filter_t 
{

    // Ligne de retard (Buffer circulaire)
    std::vector<float> delay_buffer_L, delay_buffer_R;
    size_t writeIndex_L = 0; size_t writeIndex_R = 0;
    size_t buffer_size = 0;

    // Paramètres de l'effet
    float sample_rate = 44100.0f;
    float lfo_rate_Hz = 0.25f;       // Vitesse du LFO (vitesse du balayage)
    float depth = 0.7f;         // Intensité de la modulation (0.0 à 1.0)
    float feedback = 0.3f;      // Réinjection du signal (crée un effet plus prononcé)
    float dryWet = 0.5f;        // Balance entre signal d'origine (0.0) et effet (1.0)

    // Phase de l'oscillateur (LFO)
    float lfoPhase = 0.0f;

    // Constantes de temps pour le Flanger (en secondes)
    const float maxDelaySec = 0.005f; // 5 ms max pour un flanger typique
    const float baseDelaySec = 0.002f; // 2 ms de retard de base


    inline void  set_sample_rate(float sr) 
    {
        sample_rate = sr;
        buffer_size = static_cast<int>(maxDelaySec * sample_rate) + 5;
        delay_buffer_L.resize(buffer_size, 0.0f);
        delay_buffer_R.resize(buffer_size, 0.0f);
        reset();
    }

    // Configuration des paramètres à la volée
    void set_parameters(float lfo_rate, float depthVal, float feedbackVal, float dryWetVal) {
        lfo_rate_Hz = lfo_rate;
        depth = std::clamp(depthVal, 0.0f, 1.0f);
        feedback = std::clamp(feedbackVal, -0.95f, 0.95f); // eviter l'auto-oscillation infinie
        dryWet = std::clamp(dryWetVal, 0.0f, 1.0f);
        reset();
    }

    // Traitement d'un échantillon unique (Mono)
    inline void process(float *sample_L, float *sample_R) 
    {
        // 1. Mettre à jour le LFO (Génère une onde sinusoïdale entre -1.0 et 1.0)
        float lfoOut = std::sin(lfoPhase);
        
        // Avancer la phase du LFO pour le prochain échantillon
        lfoPhase = std::clamp( lfoPhase + 2.0f * M_PI * lfo_rate_Hz / sample_rate, -2.0f * M_PI, 2.0f * M_PI);
        // 2. Calculer le temps de retard actuel (en secondes) modulé par le LFO
        // Le retard va osciller autour de baseDelaySec
        float currentDelaySec = baseDelaySec + (lfoOut * depth * 0.002f); 
        
        // Convertir le retard en nombre d'échantillons (valeur flottante)
        float delaySamples = currentDelaySec * sample_rate;

        // 3. Calculer la position de lecture dans le buffer circulaire
        float readPosition = static_cast<float>(writeIndex_L) - delaySamples;
        if (readPosition < 0.0f) 
        {
            readPosition += static_cast<float>(buffer_size);
        }

        size_t indexA = static_cast<size_t>(readPosition);
        size_t indexB = (indexA + 1) % buffer_size;
        float fraction = readPosition - static_cast<float>(indexA);
        
        float delayedSample = delay_buffer_L[indexA] + fraction * (delay_buffer_L[indexB] - delay_buffer_L[indexA]);

        // 5. Calculer le signal à écrire (Entrée + Feedback du signal retardé)
        float sampleToStore = *sample_L + (delayedSample * feedback);
        delay_buffer_L[writeIndex_L] = sampleToStore;

        // Avancer l'index d'écriture du buffer circulaire
        writeIndex_L = (++writeIndex_L) % buffer_size;
        *sample_L = ((1.0f - dryWet) * *sample_L) + (dryWet * delayedSample);

        readPosition = static_cast<float>(writeIndex_R) - delaySamples;
        if (readPosition < 0.0f) 
        {
            readPosition += static_cast<float>(buffer_size);
        }

        indexA = static_cast<size_t>(readPosition);
        indexB = (indexA + 1) % buffer_size;
        fraction = readPosition - static_cast<float>(indexA);
        
        delayedSample = delay_buffer_R[indexA] + fraction * (delay_buffer_R[indexB] - delay_buffer_R[indexA]);
        sampleToStore = *sample_R + (delayedSample * feedback);
        delay_buffer_R[writeIndex_R] = sampleToStore;

        sampleToStore = *sample_R + (delayedSample * feedback);

        // Avancer l'index d'écriture du buffer circulaire
        writeIndex_R = (++writeIndex_R) % buffer_size;      
        // 6. Mixer le signal Dry (origine) et Wet (effet)
        *sample_R = ((1.0f - dryWet) * *sample_R) + (dryWet * delayedSample);

    }

    void reset() {
        std::fill(delay_buffer_L.begin(), delay_buffer_L.end(), 0.0f);
        std::fill(delay_buffer_R.begin(), delay_buffer_R.end(), 0.0f);
        writeIndex_L = writeIndex_R = 0;
        lfoPhase = 0.0f;
    }
};




typedef void (*apply_filter_function)(float *sample_L,float *sample_R);

static std::vector<filter_combo_item_t> filters_items = {
    { "reverb", false },
    { "compressor", false },
    { "flanger", false },
};

reverb_filter_t reverb_filter;
compressor_filter_t compressor_filter;
flanger_filter_t flanger_filter;

static inline void apply_flanger(float *sample_L,float *sample_R)
{
        flanger_filter.process(sample_L,sample_R);
}

static inline void apply_reverb(float *sample_L,float *sample_R)
{

   reverb_filter.process(sample_L,sample_R);

}

static inline void apply_compressor(float *sample_L,float *sample_R)
{

   compressor_filter.process(sample_L,sample_R);

}

std::unordered_map<const char *, apply_filter_function > filters_map =
{
    {"reverb", &apply_reverb},
    {"compressor", &apply_compressor},
    {"flanger", &apply_flanger}
};

std::unordered_set<const char *> active_filters;

struct ScopeData {
    std::vector<float> circularBuffer{std::vector<float>(BUFFER_SIZE, 0.0f)};
    int writeIndex = 0;
    std::mutex mtx;
} g_scope;

// Oscillateur de test interne (Génère un sinus pur)
float g_phase = 0.0f;
const float TARGET_FREQ = 440.0f; // La phase restera stable pour n'importe quelle fréquence

static auto now()
{
        return  std::chrono::high_resolution_clock::now();
}
static double now_to_seconds()
{
       return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::time_point_cast<std::chrono::nanoseconds>( now() ).time_since_epoch()).count() * 1.0e-9;
}

static inline double elapsed_time_in_seconds(std::chrono::time_point<std::chrono::high_resolution_clock> start,
                                                 std::chrono::time_point<std::chrono::high_resolution_clock> end)
    {
        return  abs(std::chrono::duration_cast<std::chrono::microseconds>( end - start ).count())*1e-6;
    }

static std::string duration_to_hhmmss(double duration)
    {

        duration = std::fmod(duration,24.*3600.);
        size_t hh = duration/3600;
        duration = std::fmod(duration,3600.);
        size_t mm = duration/60;
        duration = std::fmod(duration,60.);
        size_t ss = duration;
        duration -= (double)ss;
        duration = round(duration*100000.);
        std::string r = std::to_string(hh)+":"+std::to_string(mm)+":"+std::to_string(ss)+"."+std::to_string(duration);
        return r;
    }


inline bool load_wav_file(std::string &path,std::vector<float> *interleaved_samples, uint *channels, uint *sample_rate)
{

    drwav_uint64 totalPCMFrameCount;

    float* pSampleData = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), channels, sample_rate, &totalPCMFrameCount, NULL);
    
    if (pSampleData == NULL)
    {
        // Error opening and reading WAV file.
        std::string msg = "an error occurred while loading file '"; msg+=path.c_str();msg+="'.";
        printf("%s \n", msg.c_str());
        return false;
    }

    // build node data
    //  copy samples to node data structure
    interleaved_samples->resize(totalPCMFrameCount* *channels);
    memcpy(interleaved_samples->data(),pSampleData,totalPCMFrameCount* *channels * sizeof(float));
    return true;
}

static bool load_flac_file(std::string &path,std::vector<float> *interleaved_samples, uint *channels, uint *sample_rate)
{

    drwav_uint64 totalPCMFrameCount;
    float* pSampleData = drflac_open_file_and_read_pcm_frames_f32(path.c_str(), channels, sample_rate, &totalPCMFrameCount, NULL);
    if (pSampleData == NULL)
    {
        // Error opening and reading WAV file.
        std::string msg = "an error occurred while loading file '"; msg+=path.c_str();msg+="'.";
        printf("%s \n", msg.c_str());
        return false;
    }

    // build node data
    //  copy samples to node data structure
    interleaved_samples->resize(totalPCMFrameCount * *channels);
    memcpy(interleaved_samples->data(),pSampleData,totalPCMFrameCount* *channels*sizeof(float));
    drwav_free(pSampleData, NULL); // free memory
    return true;
}

static inline bool load_mp3_file(std::string &path,std::vector<float> *interleaved_samples, uint *channels, uint *sample_rate)
    {

        drmp3_uint64 totalPCMFrameCount = 0;
        drmp3_config config;



        float* pSampleData = drmp3_open_file_and_read_pcm_frames_f32(path.c_str(),&config, &totalPCMFrameCount, NULL);
        if (pSampleData == NULL)
        {
            std::string msg = "an error occurred while loading file '"; msg+=path.c_str();msg+="'.";
            printf("%s \n", msg.c_str());
            return false;
        }
        *channels = config.channels;
        *sample_rate = config.sampleRate;
        // build node data
        //  copy samples to node data structure
        interleaved_samples->resize(totalPCMFrameCount*config.channels);
        memcpy(interleaved_samples->data(),pSampleData,totalPCMFrameCount* *channels *sizeof(float));
        drwav_free(pSampleData, NULL); // free memory
        return true;
    }


        // Wave, audio wave data
        typedef struct Wave
        {
            unsigned int frameCount;    // Total number of frames (considering channels)
            unsigned int sampleRate;    // Frequency (samples per second)
            unsigned int sampleSize;    // Bit depth (bits per sample): 8, 16, 32 (24 not supported)
            unsigned int channels;      // Number of channels (1-mono, 2-stereo, ...)
            void *data;                 // Buffer data pointer
        } Wave;

        // Convert wave data to desired format
        inline void WaveFormat(Wave *wave, int sampleRate, int sampleSize, int channels)
        {
            ma_format formatIn = ((wave->sampleSize == 8)? ma_format_u8 : ((wave->sampleSize == 16)? ma_format_s16 : ma_format_f32));
            ma_format formatOut = ((sampleSize == 8)? ma_format_u8 : ((sampleSize == 16)? ma_format_s16 : ma_format_f32));

            ma_uint32 frameCountIn = wave->frameCount;
            ma_uint32 frameCount = (ma_uint32)ma_convert_frames(NULL, 0, formatOut, channels, sampleRate, NULL, frameCountIn, formatIn, wave->channels, wave->sampleRate);

            if (frameCount == 0)
            {
                printf( "WAVE: Failed to get frame count for format conversion" );
                return;
            }

            void *data = malloc(frameCount*channels*(sampleSize/8));

            frameCount = (ma_uint32)ma_convert_frames(data, frameCount, formatOut, channels, sampleRate, wave->data, frameCountIn, formatIn, wave->channels, wave->sampleRate);
            if (frameCount == 0)
            {
                free(wave->data);
                wave->data = nullptr;
                printf( "WAVE: Failed format conversion");
                return;
            }

            wave->frameCount = frameCount;
            wave->sampleSize = sampleSize;
            wave->sampleRate = sampleRate;
            wave->channels = channels;

            free(wave->data);

            wave->data = data;
        }



        static inline bool ma_save_file_data(const char *fileName, void *data, int dataSize)
        {
            if (fileName != nullptr)
            {
                FILE *file = fopen(fileName, "wb");

                if (file != nullptr)
                {
                    unsigned int count = (unsigned int)fwrite(data, sizeof(unsigned char), dataSize, file);

                    if (count == 0) printf( "FILEIO: [%s] Failed to write file", fileName);
                    else if (count != dataSize) printf( "FILEIO: [%s] File partially written", fileName);
                    else printf( "FILEIO: [%s] File saved successfully", fileName);

                    fclose(file);
                }
                else
                {
                    printf( "FILEIO: [%s] Failed to open file", fileName);
                    return false;
                }
            }
            else
            {
                printf( "FILEIO: File name provided is not valid");
                return false;
            }

            return true;
        }

        // Export wave data to file
        static inline bool export_wave(Wave wave, const char *fileName)
        {
            bool success = false;

            drwav wav = { 0 };
            drwav_data_format format;
            format.container = drwav_container_riff;
            if (wave.sampleSize == 32) format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
            else format.format = DR_WAVE_FORMAT_PCM;
            format.channels = wave.channels;
            format.sampleRate = wave.sampleRate;
            format.bitsPerSample = wave.sampleSize;

            void *fileData = NULL;
            size_t fileDataSize = 0;
            success = drwav_init_memory_write(&wav, &fileData, &fileDataSize, &format, NULL);
            if (success) success = (int)drwav_write_pcm_frames(&wav, wave.frameCount, wave.data);
            drwav_result result = drwav_uninit(&wav);

            if (result == DRWAV_SUCCESS) success = ma_save_file_data(fileName, (unsigned char *)fileData, (unsigned int)fileDataSize);

            drwav_free(fileData, NULL);

                return success;
        }
        static inline bool ma_read_from_wav_file(const char *file_name, std::vector<float> *interleaved_samples, size_t *channels, size_t *sample_rate)
        {
            Wave wave = { 0 };

            // Loading file to memory
          //  int dataSize = 0;
          //  unsigned char *file_data = ma_load_file_data(file_name.toStdString().c_str(), &dataSize);

            // Loading wave from memory data
            drwav wav = { 0 };

            bool success = drwav_init_file(&wav, file_name, NULL);

            if (success)
            {
                wave.frameCount = (unsigned int)wav.totalPCMFrameCount;
                wave.sampleRate = wav.sampleRate;
                wave.sampleSize = wav.bitsPerSample;
                wave.channels = wav.channels;
                std::vector<short> samples; samples.resize(wave.frameCount* wave.channels );
                wave.data = samples.data();
                *sample_rate = wave.sampleRate;
                *channels = wave.channels;
                // NOTE: We are forcing conversion to 16bit sample size on reading

                size_t samples_size = wave.frameCount;
                if (wave.sampleSize == 8)
                {
                    drwav_read_pcm_frames(&wav,samples_size,wave.data);
                }
                drwav_uint64 frames_read = drwav_read_pcm_frames_s16(&wav, wave.frameCount, (drwav_int16 *) wave.data);
                if ( frames_read != samples_size)
                {
                    printf("file corrupted. aborted.");
                    return false;
                }
                interleaved_samples->resize(samples_size * *channels);

                short * frame = (short *)wave.data;
                for (size_t i = 0; i < samples_size ;  i += *channels)
                {
                    /*  if (wave.sampleSize == 8)
                    {
                        for (size_t k = 0; k < *channels; k++)
                        {
                            *(interleaved_samples->data() + *channels * i + k)  = (T)(((uint8_t *)(wave.data))[i + k] - 128)/128.0f;

                        }
                    }
                    else if (wave.sampleSize == 16) */
                    // {
                    for (size_t k = 0; k < *channels; k++)
                    {
                        *(interleaved_samples->data() + *channels * i + k) = (float)(frame[i + k])/32768.0f;

                      //  printf(" %i  %i   %zu  %f \n",i+k,frame[i + k], *channels * i + k , (*interleaved_samples)[*channels * i + k]);

                    }
                    // }
                    /*
                    else if (wave.sampleSize == 32)
                    {
                        for (size_t k = 0; k < *channels; k++)
                        {
                            *(interleaved_samples->data() + *channels * i + k) = ((T *)wave.data)[i +k];
                        }
                    }*/
                }

            }
            else printf("WAVE: Failed to load WAV data");

            return  success  = drwav_uninit(&wav);


          //  free(file_data);

        }

    // Fonction FFT Cooley-Tukey (identique)
inline void fft(std::vector<std::complex<float>>* a) {
    int n = a->size();
    if (n <= 1) return;
    std::vector<std::complex<float>> a0(n / 2), a1(n / 2);
    for (int i = 0; 2 * i < n; i++) {
        a0[i] = (*a)[2 * i];
        a1[i] = (*a)[2 * i + 1];
    }
    fft(&a0); fft(&a1);
    float angle = 2 * M_PI / n;
    std::complex<float> w(1), wn(std::cos(angle), -std::sin(angle));
    for (int i = 0; 2 * i < n; i++) {
        (*a)[i] = a0[i] + w * a1[i];
        (*a)[i + n / 2] = a0[i] - w * a1[i];
        w *= wn;
    }
}

constexpr float threshold = 1.;

    struct audio_file_data_t
    {
        std::string path;
        float duration;
        std::string title;
        size_t channels;
        size_t sample_rate;
        std::vector<float> samples;
    };

    std::hash<std::string> string_hasher;

    std::unordered_map<size_t, audio_file_data_t > songs;

    static void load_audio_file(const char* file_path, std::vector<float> *samples)
    {

        std::string path = file_path;

        std::thread ([](std::string file){

            auto start = std::chrono::high_resolution_clock::now();
            
            uint c = 2;
            uint s = 44100;

            error_file_loading_msg = "";
            
            size_t hash = string_hasher(file);

            if ( songs.find(hash) != songs.end() )
            {
                file_loading.store(false);
                error_file_loading.store(false);
                error_file_loading_msg = "file already loaded";
                return;
            }

            auto path = std::filesystem::path(file);
            std::string extension = path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
            std::string file_name = path.stem().string();

            int status = extension == ".wav" ? 1 : extension == ".flac" ? 2 : extension == ".mp3" ? 3 : 0;
            std::vector<float> samples;
            bool success = false;
            switch(status)
            {
            case 0:
            {
                return;
            }
            break;
            case 1:
                success = load_wav_file(file, &samples,&c, &s);
                break;
            case 2:
                success = load_flac_file(file, &samples,&c, &s);
                break;
            case 3:
                success = load_mp3_file(file, &samples,&c, &s);
            }

            if ( success )
            {
                audio_file_data_t data = 
                {
                    .path = file,
                    .duration = ( static_cast<float>(samples.size())/static_cast<float>(c)/static_cast<float>(s) ),
                    .title = path.filename().string(),
                    .channels = c,
                    .sample_rate = s
                };
                data.samples.resize(samples.size());
                memcpy(data.samples.data(), samples.data(), samples.size() * sizeof(float));

                songs.emplace(hash, std::move(data) );
                channels.store(c);
                sample_rate.store(s);
                file_loaded.store(true );
            }
            file_loading.store(false);
            error_file_loading.store(!success);

        }, path).detach();
    };

bool ImGuiStopButton(const char* str_id, ImVec2 size) 
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;


    const ImGuiID id = window->GetID(str_id);
    const ImRect bb(window->DC.CursorPos, {window->DC.CursorPos[0] + size[0],window->DC.CursorPos[1] + size[1]} );
    
    ImGui::ItemSize(size, g.Style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, id)) return false;

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);


    ImU32 bg_col = ImGui::GetColorU32((held && hovered) ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    ImU32 icon_col = ImGui::GetColorU32(ImGuiCol_Text);

    window->DrawList->AddRectFilled(bb.Min, bb.Max, bg_col, g.Style.FrameRounding);


    float padding = size.y * 0.25f; // Marges internes pour que l'icône ne colle pas aux bords
    ImVec2 stop_min(bb.Min.x + padding, bb.Min.y + padding);
    ImVec2 stop_max(bb.Max.x - padding, bb.Max.y - padding);


    window->DrawList->AddRectFilled(stop_min, stop_max, icon_col);

    return pressed;
}

bool KnobDial(const char* label, float* p_value, float v_min, float v_max, float radius = 20.0f, int major_ticks = 5, int sub_ticks_per_division = 3) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);

    float line_height = ImGui::GetTextLineHeight();
    float tick_padding = 6.0f; 
    ImVec2 total_size = ImVec2((radius + tick_padding) * 2.0f, ((radius + tick_padding) * 2.0f) + line_height + style.ItemInnerSpacing.y);
    ImVec2 pos = window->DC.CursorPos;
    ImRect total_bb(pos, ImVec2(pos.x + total_size.x, pos.y + total_size.y));

    ImGui::ItemSize(total_bb, style.ItemSpacing.y);
    if (!ImGui::ItemAdd(total_bb, id)) return false;

    ImGui::SetNextItemAllowOverlap(); 

    // Calcul du nombre total d'intervalles (divisions) pour le snapping
    int total_divisions = (major_ticks > 1) ? (major_ticks - 1) * (sub_ticks_per_division + 1) : 1;
    float range = v_max - v_min;
    float step_size = range / (float)total_divisions;

    // 1. Logique d'interaction
    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(total_bb, id, &hovered, &held);
    
    if (pressed) {
        ImGui::SetKeyboardFocusHere(-1);
    }
    bool is_focused = ImGui::IsItemFocused();
    bool value_changed = false;

    // Ajustement continu à la souris
    if (held && g.IO.MouseDelta.x != 0.0f) {
        // Sensibilité dépendante de la plage totale
        *p_value += range * 0.005f * g.IO.MouseDelta.x;
        value_changed = true;
    }

    // Ajustement par pallier d'un tick complet au clavier
    if (is_focused) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            *p_value -= step_size;
            value_changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            *p_value += step_size;
            value_changed = true;
        }
    }

    // APPLICATION DU SNAPPING (Uniquement si la valeur a bougé)
    if (value_changed) {
        // Clamping préventif
        if (*p_value < v_min) *p_value = v_min;
        if (*p_value > v_max) *p_value = v_max;

        // Formule mathématique pour forcer l'alignement sur l'étape la plus proche
        float snap_t = std::round((*p_value - v_min) / step_size);
        *p_value = v_min + (snap_t * step_size);
    }

    // 2. Configuration Géométrique
    float t = (*p_value - v_min) / range;
    float angle_min = 3.14159265f * 0.75f; 
    float angle_max = 3.14159265f * 2.25f; 
    float angle = angle_min + (angle_max - angle_min) * t;

    ImVec2 center = ImVec2(pos.x + radius + tick_padding, pos.y + radius + tick_padding);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    ImU32 col_bg = ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
    ImU32 col_active = ImGui::GetColorU32(ImGuiCol_SliderGrabActive);
    ImU32 col_text = ImGui::GetColorU32(ImGuiCol_Text);
    ImU32 col_tick = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    // 3. Rendu des Ticks
    if (major_ticks > 1) {
        for (int i = 0; i <= total_divisions; ++i) {
            float tick_t = (float)i / (float)total_divisions;
            float tick_angle = angle_min + (angle_max - angle_min) * tick_t;
            bool is_major = (i % (sub_ticks_per_division + 1) == 0);
            
            float inner_r = radius + 1.0f;
            float outer_r = radius + (is_major ? 5.0f : 3.0f);
            float thickness = is_major ? 1.5f : 1.0f;

            ImVec2 tick_start = ImVec2(center.x + std::cos(tick_angle) * inner_r, center.y + std::sin(tick_angle) * inner_r);
            ImVec2 tick_end   = ImVec2(center.x + std::cos(tick_angle) * outer_r, center.y + std::sin(tick_angle) * outer_r);
            draw_list->AddLine(tick_start, tick_end, col_tick, thickness);
        }
    }

    // 4. Rendu du corps du bouton
    if (is_focused) {
        draw_list->AddCircle(center, radius + 2.0f, ImGui::GetColorU32(ImGuiCol_NavHighlight), 32, 1.5f);
    }

    draw_list->AddCircleFilled(center, radius, col_bg, 32);

    // Arc de sélection externe actif
    draw_list->PathArcTo(center, radius - 2.0f, angle_min, angle, 32);
    draw_list->PathStroke(col_active, 0, 3.0f);

    // 5. Rendu de la valeur au centre
    char value_buf[32];
    std::snprintf(value_buf, sizeof(value_buf), "%.1f", *p_value); 
    ImVec2 value_size = ImGui::CalcTextSize(value_buf);
    ImVec2 value_pos = ImVec2(center.x - value_size.x * 0.5f, center.y - value_size.y * 0.5f);
    
    draw_list->AddText(value_pos, col_text, value_buf);

    // Rendu du label centré sous le widget
    ImVec2 label_pos = ImVec2(center.x - ImGui::CalcTextSize(label).x * 0.5f, pos.y + ((radius + tick_padding) * 2.0f) + style.ItemInnerSpacing.y);
    draw_list->AddText(label_pos, col_text, label);

    return value_changed;
}

bool ImGuiPlayButton(const char* label_id, ImVec2 size) {

    bool pressed = ImGui::Button(label_id, size);

    ImVec2 pos_min = ImGui::GetItemRectMin();
    ImVec2 pos_max = ImGui::GetItemRectMax();

    float padding_x = size.x * 0.25f;
    float padding_y = size.y * 0.25f;

    ImVec2 p1(pos_min.x + padding_x, pos_min.y + padding_y);           // Sommet haut-gauche
    ImVec2 p2(pos_min.x + padding_x, pos_max.y - padding_y);           // Sommet bas-gauche
    ImVec2 p3(pos_max.x - padding_x, pos_min.y + (size.y * 0.5f));     // Sommet pointe droite

    ImU32 icon_col = ImGui::GetColorU32(ImGuiCol_Text);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddTriangleFilled(p1, p2, p3, icon_col);

    return pressed;
}


// Filtre de lissage (Passe-bas) pour aider l'autocorrélation sur les signaux très bruités
std::vector<float> ApplyPreFilter(const std::vector<float> *input) {
    std::vector<float> filtered(input->size(), 0.0f);
    float lastValue = 0.0f;
    const float alpha = 0.3f; // Coupe les hautes fréquences agressives
    
    for (size_t i = 0; i < input->size(); ++i) {
        lastValue = alpha * (*input)[i] + (1.0f - alpha) * lastValue;
        filtered[i] = lastValue;
    }
    return filtered;
}

// Extraction des données avec algorithme de stabilisation de phase (Trigger)
std::vector<float> GetTriggeredSamples(float threshold, bool risingEdge) {
    std::lock_guard<std::mutex> lock(g_scope.mtx);
    std::vector<float> output(DISPLAY_SAMPLES, 0.0f);

    // On commence la recherche un peu en arrière du curseur d'écriture actuel
    int triggerIndex = (g_scope.writeIndex - DISPLAY_SAMPLES * 2 + BUFFER_SIZE) % BUFFER_SIZE;
    bool triggerFound = false;

    // Recherche du front (Edge Match)
    for (int i = 0; i < BUFFER_SIZE - DISPLAY_SAMPLES; ++i) {
        int idx0 = (triggerIndex + i) % BUFFER_SIZE;
        int idx1 = (idx0 + 1) % BUFFER_SIZE;

        float s0 = g_scope.circularBuffer[idx0];
        float s1 = g_scope.circularBuffer[idx1];

        if (risingEdge) {
            if (s0 <= threshold && s1 > threshold) { // Front montant
                triggerIndex = idx0;
                triggerFound = true;
                break;
            }
        } else {
            if (s0 >= threshold && s1 < threshold) { // Front descendant
                triggerIndex = idx0;
                triggerFound = true;
                break;
            }
        }
    }

    // Si aucun trigger n'est trouvé, on prend les derniers échantillons bruts
    if (!triggerFound) {
        triggerIndex = (g_scope.writeIndex - DISPLAY_SAMPLES + BUFFER_SIZE) % BUFFER_SIZE;
    }

    // Extraction de la fenêtre synchronisée
    for (int i = 0; i < DISPLAY_SAMPLES; ++i) {
        output[i] = g_scope.circularBuffer[(triggerIndex + i) % BUFFER_SIZE];
    }

    return output;
}



int analysisSize = DISPLAY_SAMPLES + MAX_LAG;

std::vector<float> rawLeft(BUFFER_SIZE/2);
std::vector<float> rawRight(BUFFER_SIZE/2);

std::vector<float> output_left(DISPLAY_SAMPLES);
std::vector<float> output_right(DISPLAY_SAMPLES);



inline void  get_autocorrected_samples() 
{

    for (size_t k = 0; k < buffer.size()/2;k++)
    {
        rawLeft[k] = buffer[2 * k ], rawRight[k] = buffer[2 * k + 1];
    }



    // 2. Calcul de l'autocorrélation sur le canal GAUCHE uniquement (Référence)
    std::vector<float> diffFunction(MAX_LAG, 0.0f);
    float globalMinDiff = 1e9f;
    int bestLag = -1;

    for (int lag = MIN_LAG; lag < MAX_LAG; ++lag) {
        float currentDiff = 0.0f;
        for (int i = 0; i < DISPLAY_SAMPLES; ++i) {
            float diff = rawLeft[i] - rawLeft[i + lag]; // Analyse basée sur Gauche
            currentDiff += diff * diff;
        }
        diffFunction[lag] = currentDiff;
    }

    // Trouver le premier creux local
    for (int lag = MIN_LAG + 1; lag < MAX_LAG - 1; ++lag) {
        if (diffFunction[lag] < diffFunction[lag - 1] && diffFunction[lag] < diffFunction[lag + 1]) {
            if (diffFunction[lag] < globalMinDiff * 1.5f || bestLag == -1) {
                globalMinDiff = diffFunction[lag];
                bestLag = lag;
                if (globalMinDiff < 0.001f) break;
            }
        }
    }

    int renderStartIndex = (bestLag != -1) ? bestLag : MAX_LAG;

    // 3. Copie des deux canaux synchronisés avec le MEME point de départ
    for (int i = 0; i < DISPLAY_SAMPLES; ++i) {
        output_left[i]  = rawLeft[renderStartIndex + i];
        output_right[i] = rawRight[renderStartIndex + i];
    }

}

 int REF_SIZE = BUFFER_SIZE/4;         // Taille du motif stable recherché


 int SEARCH_SIZE = BUFFER_SIZE/2;     // Fenêtre globale d'analyse audio

std::vector<float> window_hann;
std::vector<float> *generate_hann_window(int size) 
{
    window_hann.resize(size);
    for (int i = 0; i < size; ++i) {
        window_hann[i] = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (size - 1)));
    }
    return &window_hann;
}

std::vector<float> window_hamming;
std::vector<float> *generate_hamming_window(int size) 
{
    window_hamming.resize(size);
    for (int i = 0; i < size; ++i) {
        window_hamming[i] = 0.54f - 0.46f * std::cos(2.0f * M_PI * i / (size - 1));
    }
    return &window_hamming;
}

/**
 * @brief Génère les coefficients d'une fenêtre de Blackman exacte.
 * Formule : w(i) = 0.42 - 0.5 * cos(2*pi*i / (N-1)) + 0.08 * cos(4*pi*i / (N-1))
 */
std::vector<float> window_blackman;
std::vector<float> *generate_blackman_window(int size) 
{
    window_blackman.resize(size);
    for (int i = 0; i < size; ++i) {
        float alpha = 2.0f * M_PI * i / (size - 1);
        window_blackman[i] = 0.42f - 0.50f * std::cos(alpha) + 0.08f * std::cos(2.0f * alpha);
    }
    return &window_blackman;
}

 enum class window_type  {hann,hamming,blackman};

/**
 * @brief Algorithme d'alignement de Corrscope basé sur la corrélation croisée FFT.
 */

    struct trigger_t {
        int best_offset_left;
        float max_correlation_left;
        int best_offset_right;
        float max_correlation_right;
    };

    inline int next_power_of_two(int n) 
    {
        int count = 1;
        if (n && !(n & (n - 1))) return n;
        while (count < n) count <<= 1;
        return count;
    }

    int fftSize = next_power_of_two(REF_SIZE + SEARCH_SIZE);

  std::vector<float>  history_left = std::vector<float>(REF_SIZE, 0.0f),  history_right = std::vector<float>(REF_SIZE, 0.0f), 
  search_left= std::vector<float>(SEARCH_SIZE, 0.0f), search_right= std::vector<float>(SEARCH_SIZE, 0.0f);
  std::vector<float> *current_window_type = nullptr;
  std::vector<SDL_FPoint> fft_cc_render_points_left(REF_SIZE);
    std::vector<SDL_FPoint> fft_cc_render_points_right(REF_SIZE);

    std::vector<kiss_fft_cpx> in_ref_left = std::vector<kiss_fft_cpx>(fftSize, {0.0f, 0.0f});
     std::vector<kiss_fft_cpx> in_ref_right = std::vector<kiss_fft_cpx>(fftSize, {0.0f, 0.0f});

    std::vector<kiss_fft_cpx> in_search_left = std::vector<kiss_fft_cpx>(fftSize, {0.0f, 0.0f});
    std::vector<kiss_fft_cpx> in_search_right = std::vector<kiss_fft_cpx>(fftSize, {0.0f, 0.0f});

        std::vector<kiss_fft_cpx> out_ref_left = std::vector<kiss_fft_cpx>(fftSize, {0.0f, 0.0f});
    std::vector<kiss_fft_cpx> out_ref_right    = std::vector<kiss_fft_cpx>(fftSize, {0.0f, 0.0f});

    std::vector<kiss_fft_cpx> out_search_left  = std::vector<kiss_fft_cpx>(fftSize, {0.0f, 0.0f});
    std::vector<kiss_fft_cpx> out_search_right = std::vector<kiss_fft_cpx>(fftSize, {0.0f, 0.0f});

    std::vector<kiss_fft_cpx> out_freq_product_left = std::vector<kiss_fft_cpx>(fftSize, {0.0f, 0.0f});
    std::vector<kiss_fft_cpx> out_time_left_result  = std::vector<kiss_fft_cpx>(fftSize, {0.0f, 0.0f});

    std::vector<kiss_fft_cpx> out_freq_product_right = std::vector<kiss_fft_cpx>(fftSize, {0.0f, 0.0f});
    std::vector<kiss_fft_cpx> out_time_right_result  = std::vector<kiss_fft_cpx>(fftSize, {0.0f, 0.0f});



trigger_t find_corrscope_trigger() 
{

    kiss_fft_cfg forward_cfg_left = kiss_fft_alloc(fftSize, 0, nullptr, nullptr);
    kiss_fft_cfg inverse_cfg_left = kiss_fft_alloc(fftSize, 1, nullptr, nullptr);

    kiss_fft_cfg forward_cfg_right = kiss_fft_alloc(fftSize, 0, nullptr, nullptr);
    kiss_fft_cfg inverse_cfg_right = kiss_fft_alloc(fftSize, 1, nullptr, nullptr);

    // Appliquer la fenêtre de Hann et inverser temporellement pour la corrélation
    for (int i = 0; i < REF_SIZE; ++i) {
        in_ref_left[i].r  = history_left [REF_SIZE - 1 - i] * (*current_window_type)[i]; 
        in_ref_right[i].r = history_right [REF_SIZE - 1 - i] * (*current_window_type)[i]; 

    }
    for (int i = 0; i < SEARCH_SIZE; ++i) {
        in_search_left[i].r = search_left[i];
        in_search_right[i].r = search_right[i];
    }

    kiss_fft(forward_cfg_left, in_ref_left.data(), out_ref_left.data());
    kiss_fft(forward_cfg_left, in_search_left.data(), out_search_left.data());

    kiss_fft(forward_cfg_right, in_ref_right.data(), out_ref_right.data());
    kiss_fft(forward_cfg_right, in_search_right.data(), out_search_right.data());

    // Multiplication complexe
    for (int i = 0; i < fftSize; ++i) {
        out_freq_product_left[i].r = out_ref_left[i].r * out_search_left[i].r - out_ref_left[i].i * out_search_left[i].i;
        out_freq_product_left[i].i = out_ref_left[i].r * out_search_left[i].i + out_ref_left[i].i * out_search_left[i].r;
        out_freq_product_right[i].r = out_ref_right[i].r * out_search_right[i].r - out_ref_right[i].i * out_search_right[i].i;
        out_freq_product_right[i].i = out_ref_right[i].r * out_search_right[i].i + out_ref_right[i].i * out_search_right[i].r;
    }

    kiss_fft(inverse_cfg_left, out_freq_product_left.data(), out_time_left_result.data());

    int best_offset_left = 0, best_offset_right = 0;
    float max_val_left = -std::numeric_limits<float>::infinity();
    float max_val_right = -std::numeric_limits<float>::infinity();

    int valid_start = REF_SIZE - 1; 
    int valid_end = SEARCH_SIZE; 
    float currentVal = 0;
    // Recherche de l'index du pic maximal
    for (int i = valid_start; i < valid_end; ++i) 
    {
        currentVal = out_time_left_result[i].r / fftSize; 
        if (currentVal > max_val_left) 
        {
            max_val_left = currentVal;
            best_offset_left = i - valid_start;
        }
        currentVal = out_time_right_result[i].r / fftSize; 
        if (currentVal > max_val_left) 
        {
            max_val_right = currentVal;
            best_offset_right = i - valid_start;
        }
    }

    free(forward_cfg_left);
    free(inverse_cfg_left);
    free(forward_cfg_right);
    free(inverse_cfg_right);

    return {best_offset_left, max_val_left,best_offset_right, max_val_right};
}

inline int apply_zero_crossing_post_trigger(const std::vector<float> *searchRegion, int fftOffset, int radius) {

    
    // S'assurer de ne pas déborder de la zone de recherche
    int startSearch = std::max(0, fftOffset - radius);
    int endSearch   = std::min(SEARCH_SIZE - 2, fftOffset + radius);

    int best_offset = fftOffset;
    float minDistanceToZero = std::numeric_limits<float>::infinity();

    // Recherche d'un passage par zéro de type flanc montant : échantillon[i] <= 0 et échantillon[i+1] > 0
    for (int i = startSearch; i < endSearch; ++i) {
        if ( (*searchRegion)[i] <= 0.0f && (*searchRegion)[i + 1] > 0.0f) {
            // Calcule la distance par rapport à l'estimation brute de la FFT
            float distance = std::abs(i - fftOffset);
            if (distance < minDistanceToZero) {
                minDistanceToZero = distance;
                best_offset = i; // Verrouillage de la position exacte
            }
        }
    }

    return best_offset;
}

float sample_L, sample_R, sample;
inline void fft_cross_correlation(float center_y_left,float center_y_right,float scale)
{

    // Extraction de la région de recherche de notre flux
        
        for (int i = 0; i < SEARCH_SIZE; ++i) 
        {
            sample_L = search_left[i] ;
            sample_R = search_right[i] ;
            search_left[i]  = playback.load() == true ? buffer[2 * i] : sample_L * 0.995 ;
            search_right[i] = playback.load() == true ?buffer[2 * i + 1] : sample_R * 0.995 ;
        }
        

        trigger_t trigger = playback.load() == true ?  find_corrscope_trigger() : trigger_t{0,0.f,0,0.f};

        int final_offset_left  = apply_zero_crossing_post_trigger(&search_left, trigger.best_offset_left, 32);
        int final_offset_right = apply_zero_crossing_post_trigger(&search_right, trigger.best_offset_right, 32);


        float xStep = (float)WINDOW_WIDTH / REF_SIZE;


        int audio_index;
        float alpha = 0.15f;    
        for (int i = 0; i < REF_SIZE; ++i) 
        {

                audio_index = final_offset_left + i;
                // Sécurité contre les débordements de tampons
                sample = (audio_index < (int)search_left.size()) ? search_left[audio_index] : 0.0f;

                fft_cc_render_points_left[i].x = i * xStep;
                fft_cc_render_points_left[i].y = center_y_left - (sample * scale);
                history_left[i] = sample;
                history_left[i] = (history_left[i] * (1.0f - alpha)) + (sample * alpha);

                audio_index = final_offset_right + i;
                sample = (audio_index < (int)search_right.size()) ? search_right[audio_index] : 0.0f;
                fft_cc_render_points_right[i].x = i * xStep;
                fft_cc_render_points_right[i].y = center_y_right - (sample * scale);
                history_right[i] = sample;
                history_right[i] = (history_right[i] * (1.0f - alpha)) + (sample * alpha);
        }

        

}

double total_playing_time = 0;
inline void update_data_from_loaded_file(size_t song_hash)
{
    // reset sample rate for filter
    reverb_filter.set_sample_rate(sample_rate.load() );
    compressor_filter.set_sample_rate(sample_rate.load() );
    flanger_filter.set_sample_rate(sample_rate.load() );
    // copy audio data to samples vector
    samples.resize(songs[song_hash].samples.size());
    memcpy(samples.data(),songs[song_hash].samples.data(), songs[song_hash].samples.size() * sizeof(float) );
    total_playing_time = samples.size()/channels / sample_rate;
}

int main(int argc, char* argv[]) 
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) 
    {
        printf("Erreur d'initialisation SDL: %s\n", SDL_GetError());
        return -1;
    }

    std::unordered_map < window_type, std::vector<float> *> windows_types = 
    {
        {window_type::hann,      generate_hann_window(REF_SIZE)},    
        {window_type::hamming,   generate_hamming_window(REF_SIZE)},
        {window_type::blackman, generate_blackman_window(REF_SIZE)}
    };

    current_window_type = windows_types[window_type::hann];

    SDL_Window* window = SDL_CreateWindow("Spectrogramme 2D - SDL3", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_RESIZABLE);
    
    if (!window) {
        printf("Erreur de création de la fenêtre: %s\n", SDL_GetError());
        return -1;
    }
    
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) 
    {
        return -1;
    }

    SDL_SetRenderVSync(renderer, 1); 

    SDL_Texture*  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, WINDOW_WIDTH, WINDOW_HEIGHT);

    SDL_Window* window_ui = SDL_CreateWindow("Controles", 500, 400, SDL_WINDOW_TRANSPARENT );
    SDL_Renderer* renderer_ui = SDL_CreateRenderer(window_ui, NULL);
    if (!renderer_ui) return -1;

    // Forcer le positionnement initial de la fenêtre UI sur le bureau (ex: x=100, y=100)
    SDL_SetWindowPosition(window_ui, 100, 100);

    //  Initialiser le contexte Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Touches clavier
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Manette
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; 


    static ImGui::FileBrowser fileDialog(ImGuiFileBrowserFlags_CloseOnEsc);

    fileDialog.SetTitle("Choisir un fichier audio (.wav, .flac, .mp3)");
    fileDialog.SetTypeFilters({ ".wav", ".mp3", ".flac" });

// Variable pour stocker le chemin du fichier audio ou de configuration choisi
    static std::string selected_file_path = "Aucun fichier sélectionné";


    // 3. Configurer les styles d'ImGui (ex: thème sombre)
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    ImGui_ImplSDL3_InitForSDLRenderer(window_ui, renderer_ui);
    ImGui_ImplSDLRenderer3_Init(renderer_ui);

    SDL_Surface *surface =  SDL_GetWindowSurface(window);

    int pitch;
    void *pixels;
    SDL_LockTexture(texture, nullptr, &pixels, &pitch);
    SDL_UnlockTexture(texture);
    
    SDL_Event event;
    
    std::vector<float> signal(256,0.); 
    std::deque<float> signal_queue(256,0.); 
    
    //    size_t channels = 0; 
   // double sample_rate = 44100;

    int play_position;


        SDL_AudioStream* playback_stream =nullptr; 
        SDL_AudioSpec spec{ SDL_AUDIO_F32, channels.load(), sample_rate.load() };
        SDL_AudioStream* capture_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, NULL,NULL);
       
        const int minimum_audio = ( sample_rate * sizeof(float) ) / 2;  //  Half of samples per seconds 

                // 1. Liste des options disponibles pour l'analyseur
        const char* oscilloscope_visualizer_style[] = { 
            "trigger", 
            "autocorrelation"
            ,"fft cross-correlation"
        };

        const char* oscilloscope_window_type[] = { 
            "hann", 
            "hamming"
            ,"blackman"
        };

         std::unordered_map<const char*,window_type> window_name_to_type = 
         { 
            {"hann", window_type::hann},
            {"hamming", window_type::hamming},
            {"blackman", window_type::blackman}
        };

    bool running = true;


    float pitch_value = 1.;


    static int current_visualizer_style_idx = 0; 

    static int current_window_type_idx = 0; 

    size_t song_hash_selected = -1;
    size_t id_a_supprimer = -1;




    float gain_value = -20.;

    std::vector<float> drawSamples  = std::vector<float>(DISPLAY_SAMPLES,0.) ;

        float hWidth = 800.0f / (DISPLAY_SAMPLES - 1);
        float centerY = 300.0f;
        float scale = 200.0f; // Hauteur de l'onde à l'écran
        float center_y_left = 150.0f;
        float center_y_right = 450.0f;

        std::string filters_preview_text = "";
        int filters_selected_count = 0;





    while (running) {
        while (SDL_PollEvent(&event)) 
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if ( event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED )
            {
                Uint32 closed_id = event.window.windowID;
                if (closed_id == SDL_GetWindowID(window)) {
                    running = false; // Fermer tout si la fenêtre principale se ferme
                }

            }
        }


        if (playback.load() == true && SDL_GetAudioStreamQueued(playback_stream) < minimum_audio)
        {
            // this will feed 1024 samples each frame until we get to our maximum. 
            // generate samples from grooves 
            next_cursor.store( std::min(samples.size() - 1, samples_cursor.load() + buffer.size() ) );
            memcpy(buffer.data(), (const void *)(samples.data()+samples_cursor), (next_cursor - samples_cursor) * sizeof(float) );

            for (size_t k = 0; k <  buffer.size() ; k+=2 )
            {
                sample_L = buffer[k];
                sample_R = buffer[k + 1];
                for ( auto it = active_filters.begin(); it != active_filters.end();++it)
                {
                    filters_map[*it](&sample_L, &sample_R);
                }
                buffer[k] = sample_L;
                buffer[k + 1] = sample_R;

            }
            // feed the new data to the stream. It will queue at the end, and trickle out as the hardware needs more data. 
            SDL_PutAudioStreamData(playback_stream, buffer.data(), (next_cursor - samples_cursor) * sizeof(float) );
            samples_cursor.store( next_cursor );
            current_playing_time = samples_cursor/sample_rate/channels;
            if ( samples_cursor >= samples.size() - 1 )
            {
                playback.store(false);
            }

        }
        // Dessin de l'interface
        SDL_SetRenderDrawColor(renderer, 10, 15, 20, 255);
        SDL_RenderClear(renderer);
        // Dessin de la grille de l'oscilloscope
        SDL_SetRenderDrawColor(renderer, 40, 50, 60, 255);
        for (int i = 1; i < 8; ++i) {
            SDL_RenderLine(renderer, (800 / 8) * i, 0, (800 / 8) * i, 600);
            SDL_RenderLine(renderer, 0, (600 / 8) * i, 800, (600 / 8) * i);
        }
            switch(current_visualizer_style_idx)
            {
                case 0:
                {
                    GetTriggeredSamples(0.0f, true);
                }
                break;
                case 1:
                {
                    if ( playback.load() == true)
                    {
                        get_autocorrected_samples();
                    }
                            
                    SDL_SetRenderDrawColor(renderer, 0, 255, 128, 255);
                    for (int i = 0; i < DISPLAY_SAMPLES - 1; ++i) {
                        SDL_RenderLine(renderer, i * hWidth, center_y_left - ( (output_left[i]*=0.995) * scale), 
                                                (i + 1) * hWidth, center_y_left - ( (output_left[i + 1] *=0.995)* scale));
                    }
                    SDL_SetRenderDrawColor(renderer, 255, 128, 0, 255); // Orange pour la Droite

                    for (int i = 0; i < DISPLAY_SAMPLES - 1; ++i) 
                    {
                        SDL_RenderLine(renderer, i * hWidth, center_y_right - ( (output_right[i]*=0.995) * scale), 
                                                (i + 1) * hWidth, center_y_right - ( (output_right[i + 1]*=0.995) * scale));
                    }
                }
                break;
                case 2:
                {
                    fft_cross_correlation(center_y_left, center_y_right, scale);
                    // Dessin des axes horizontaux de référence (Gris discret)
                    SDL_SetRenderDrawColor(renderer, 35, 40, 45, 255);
                    SDL_RenderLine(renderer, 0.0f, center_y_left, (float)WINDOW_WIDTH, center_y_left);
                    SDL_RenderLine(renderer, 0.0f, center_y_right, (float)WINDOW_WIDTH, center_y_right);

                    // Dessin du Canal Gauche (Vert Émeraude)
                    SDL_SetRenderDrawColor(renderer, 0, 255, 150, 255);
                    SDL_RenderLines(renderer, fft_cc_render_points_left.data(), REF_SIZE);

                    // Dessin du Canal Droite (Magenta / Rose)
                    SDL_SetRenderDrawColor(renderer, 255, 0, 150, 255);
                    SDL_RenderLines(renderer, fft_cc_render_points_right.data(), REF_SIZE);

                }
                break;
            }
        

        SDL_RenderPresent(renderer);


        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        // Construit l'interface pour qu'elle prenne toute la place de la fenêtre UI
        int ui_w, ui_h;
        SDL_GetWindowSize(window_ui, &ui_w, &ui_h);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)ui_w, (float)ui_h));

        // Fenêtre ImGui fixe (sans titre ni bordure interne car c'est la fenêtre OS qui fait office de cadre)
        ImGuiWindowFlags ui_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
        
        ImGui::Begin("menu", nullptr, ui_flags);
        ImGui::Separator();


        // Section Configuration
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "oscilloscope visualizer");
        ImGui::Separator();

        ImGui::Combo("oscilloscope visualizer", &current_visualizer_style_idx, oscilloscope_visualizer_style, IM_ARRAYSIZE(oscilloscope_visualizer_style));
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Combo("oscilloscope window", &current_window_type_idx, oscilloscope_window_type, IM_ARRAYSIZE(oscilloscope_window_type));


        current_window_type = windows_types[ window_name_to_type [oscilloscope_window_type[current_window_type_idx]] ];
        
        ImGui::Spacing();
        ImGui::Separator();
        if( ImGui::SliderFloat("Gain (dB)", &gain_value, -90, 0.) )
        {
            gain_db.store(gain_value);
            gain.store( std::pow( 10.f,gain_db/20.f ) );

        }
        ImGui::Separator();
        ImGui::Spacing();
        if ( KnobDial("pitch",&pitch_value,0.25,5.) )
        {
            //pitch shift
        }
        ImGui::SameLine();

        // filters combox box with checkbox items
        filters_selected_count = 0;
        for (const auto& item : filters_items) {
            if (item.is_selected) {
                filters_selected_count++;
                if (!filters_preview_text.empty()) filters_preview_text += ", ";
                filters_preview_text += item.label;
            }
        }

        if (filters_selected_count == 0) {
            filters_preview_text = "Aucun filtre sélectionné";
        } else if (filters_preview_text.length() > 25) { // Évite que le texte dépasse du widget
            filters_preview_text = std::to_string(filters_selected_count) + " éléments cochés";
        }

        // 2. Rendu du Combo avec l'aperçu dynamique
        if (ImGui::BeginCombo("Filtres", filters_preview_text.c_str())) {
            for (size_t i = 0; i < filters_items.size(); ++i) {
                ImGui::PushID((int)i);
                if (ImGui::Checkbox(filters_items[i].label, &filters_items[i].is_selected)) 
                {
                    if ( filters_items[i].is_selected )
                    {
                        active_filters.emplace(filters_items[i].label);
                    }
                    else
                    {
                        auto it = active_filters.find(filters_items[i].label);
                        if( it != active_filters.end() )
                        {
                            active_filters.erase(it);
                        }
                    }
                }
                
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }


        ImGui::Separator();
        
        // if capture to playback not selected
        if (playback.load() == false )
        {
            ImGui::Text("choose audio file :");
            ImGui::TextUnformatted(selected_file_path.c_str());
            // 2. Bouton pour ouvrir l'explorateur
            if (ImGui::Button("open audio file...")) {
                // Configurer le titre et les extensions autorisées
                
                
                // Ouvrir la boîte de dialogue
                fileDialog.Open();
            }

            if (fileDialog.HasSelected()) 
            {

                selected_file_path = fileDialog.GetSelected().string();
                
                if ( !selected_file_path.empty() )
                {
                    file_loading.store(true);
                    current_playing_time = 0.;
                    std::thread( [](std::string file, std::vector<float> *samples, SDL_AudioStream* playback_stream )
                    {
                        load_audio_file(file.c_str(),samples);
                    },
                    selected_file_path,&samples,playback_stream).detach();
                }
                close_dialog.store(true);
                fileDialog.ClearSelected(); // Réinitialiser le dialogue
            }

            if (file_loading)
            {
                std::string m =std::string("loading file ...")+selected_file_path;
                ImGui::Text(m.c_str());
            }

            if (error_file_loading.load() == true )
            {
                std::string m =std::string("error: unable to load file ...")+selected_file_path;
                if ( !error_file_loading_msg.empty())
                {
                    m = error_file_loading_msg;
                }
                ImGui::Text(m.c_str());
            }

            if (close_dialog.load() )
            {
                fileDialog.Close();
                close_dialog.store(false);
            }

            if (fileDialog.IsOpened()) 
            {
                ImGui::SameLine();
                if (ImGui::Button("close")) 
                {
                    fileDialog.Close();
                }
                // Récupérer la taille actuelle de votre fenêtre SDL d'interface (window_ui)
                int ui_w, ui_h;
                SDL_GetWindowSize(window_ui, &ui_w, &ui_h);
                
                // On force la fenêtre de l'explorateur à être légèrement plus petite 
                // que la fenêtre système pour que la croix 'X' reste accessible.
                ImGui::SetNextWindowSize(ImVec2((float)ui_w - 40.0f, (float)ui_h - 60.0f));
                ImGui::SetNextWindowPos(ImVec2(20.0f, 40.0f));
            }

            fileDialog.Display();
        }
        if ( file_loaded.load() == true)
        {
            ImGui::Separator();

            if ( playback.load() == false )
            {
                ImGui::Text("file ready to play");
            }
            else
            { 
                std::string c = duration_to_hhmmss(current_playing_time);
                std::string d = std::string("playing file: ")+c+std::string("/")+duration_to_hhmmss(total_playing_time);
                ImGui::Text(d.c_str());
                bool change_play_position = ImGui::SliderInt("##playing_position", &play_position, 0, samples.size() - 1,c.c_str(),0);
                if (!change_play_position)
                {
                    play_position = samples_cursor.load();
                }
                else 
                {
                    audio_cursor_stream.store( play_position );
                    samples_cursor.store( play_position );
                    current_playing_time = play_position/sample_rate/channels;
                }
                
                if (samples_cursor.load() < samples.size() )
                {
                    signal_queue.push_back( *(samples.data() + samples_cursor.load() ) );
                    signal_queue.pop_front();
                    signal = std::vector<float>(signal_queue.begin(),signal_queue.end() );
                    ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(1.0f, 0.4f, 0.4f, 1.0f)); // Courbe Rouge
                    ImGui::PlotLines("Samples", signal.data(), 256,0,"samples",-1.1f, 1.1f, ImVec2(0, 50));
                    ImGui::PopStyleColor();
                }
            }
            
            ImGui::Spacing(); 
            ImGui::Dummy(ImVec2(0.0f, 25.0f)); 
            ImGui::Separator();
            // Bouton Play

            if ( !songs.empty() && playback.load() == false && ImGuiPlayButton("##PlayBtn", ImVec2(32.0f, 32.0f)) ) 
            {
                play_position = 0;
                last_audio_cursor_stream = 0;
                audio_cursor_stream = 0;
                samples_cursor = 0;
                next_cursor = 0;
                SDL_ResumeAudioStreamDevice(playback_stream);
                size_t song_hash = songs.begin()->first ;
                song_hash = song_hash_selected != -1 ? song_hash = song_hash_selected : song_hash;
                update_data_from_loaded_file(song_hash);
                SDL_AudioSpec spec{ SDL_AUDIO_F32, channels.load(), sample_rate.load() };
                if ( playback_stream != nullptr)
                {
                    SDL_PauseAudioStreamDevice(playback_stream);
                    SDL_ClearAudioStream(playback_stream);
                    SDL_DestroyAudioStream(playback_stream);
                }
                playback_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL,NULL);
                if ( playback_stream == nullptr)
                {
                    error_file_loading_msg = "audio output could not be initialized";
                }
                else
                {
                    SDL_ResumeAudioStreamDevice(playback_stream);
                    playback.store(true);
                }
            }
            ImGui::SameLine();
            if ( ImGuiStopButton("##AudioStop", ImVec2(32.0f, 32.0f))) 
            {
                last_audio_cursor_stream = 0;
                audio_cursor_stream = 0;
                samples_cursor = 0;
                next_cursor = 0;
                SDL_PauseAudioStreamDevice(playback_stream);
                SDL_ClearAudioStream(playback_stream); 
                playback.store(false);
            }

            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lancer la lecture");

            if (ImGui::IsItemHovered()) 
            {
                ImGui::SetTooltip("stop playback");
            }
            ImGui::Separator();
            for (auto const& [id, element] : songs)
            {
                // 1. Configuration des drapeaux (Flags)
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
                
                // Mettre en surbrillance si cet ID est sélectionné
                if (song_hash_selected == id) {
                    flags |= ImGuiTreeNodeFlags_Selected;
                }

                // Optionnel : Si vous n'avez pas d'enfants à afficher pour cet élément, 
                // vous pouvez le traiter comme une feuille directement :
                // flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

                // 2. Rendu du nœud en utilisant l'ID unique de la map
                // On caste l'ID (uint64_t) en (void*)(uintptr_t) pour le système d'ID d'ImGui
                bool node_ouvert = ImGui::TreeNodeEx((void*)(uintptr_t)id, flags, "%s", element.title.c_str());

                // 3. Gestion de la sélection au clic
                // !ImGui::IsItemToggledOpen() évite de sélectionner l'élément si on clique juste sur la flèche
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                    song_hash_selected = id;
                }

                // --- DEBUT DU MENU CONTEXTUEL (CLIC DROIT) ---
                // Cette fonction s'associe automatiquement au dernier TreeNodeEx affiché ci-dessus
                if (ImGui::BeginPopupContextItem()) 
                {
                    // Option de sélection rapide au clic droit
                    if (ImGui::MenuItem("Sélectionner")) 
                    {
                        song_hash_selected = id;
                    }
                    
                    ImGui::Separator();

                    // Option de suppression stylisée en rouge
                    if (ImGui::MenuItem("Supprimer", nullptr, false, true)) {
                        id_a_supprimer = id; // On mémorise l'ID, on NE supprime PAS tout de suite
                    }

                    ImGui::EndPopup();
                }

                // 4. Affichage du contenu intérieur si le nœud est développé
                if (node_ouvert)
                {
                    // On affiche et modifie directement les données de la structure

                    ImGui::Text("path :      %s", songs[id].path.c_str());
                    ImGui::Text("duration    %s", duration_to_hhmmss((double) songs[id].duration).c_str() );
                    ImGui::Text("channels    %zu", songs[id].channels);
                    ImGui::Text("sample rate %zu", songs[id].sample_rate);
                    
                    // Obligatoire si node_ouvert est vrai et que NoTreePushOnOpen n'est pas utilisé
                    ImGui::TreePop(); 
                }
            }
            // 2. SUPPRESSION SÉCURISÉE (En dehors de la boucle de rendu)
            if (id_a_supprimer != -1) 
            {
                songs.erase(id_a_supprimer);
                
                // Si l'élément supprimé était celui sélectionné, on réinitialise la sélection
                if (song_hash_selected == id_a_supprimer) {
                    song_hash_selected = -1; 
                }
                
                id_a_supprimer = -1; // Réinitialisation
            }
        }

        ImGui::End();

        // 3. Rendu de la scène
        ImGui::Render(); // Calcule les géométries d'ImGui

        SDL_SetRenderDrawColor(renderer_ui, 45, 45, 45, 255);  // Fond gris moyen
        SDL_RenderClear(renderer_ui);
        // On force le dessin d'ImGui sur le renderer de la deuxième fenêtre
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_ui);
        SDL_RenderPresent(renderer_ui);

    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer_ui);

    SDL_DestroyWindow(window_ui);

    SDL_DestroyAudioStream(playback_stream);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}