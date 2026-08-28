#include <thread>
#include <chrono>
#include <functional> // Requis pour std::hash
#include <unordered_map>
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

#include "spectrogram_renderer.h"
#include "imfilebrowser.h"

#include <SDL3/SDL.h>
#include <vector>
#include <mutex>
#include <cmath>
#include <iostream>

// Configuration Audio / Visuelle

 int BUFFER_SIZE = 4096;       // Taille du tampon circulaire
 int DISPLAY_SAMPLES = 512;    // Nombre d'échantillons visibles à l'écran

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
alignas(64) std::atomic<bool> update_playback_stream = true;
alignas(64) std::atomic<bool> file_loading = false;
alignas(64) std::atomic<bool> error_file_loading = false;
alignas(64) std::atomic<bool> close_dialog = false;

size_t last_audio_cursor_stream = 0;
alignas(64) std::atomic<size_t> audio_cursor_stream = 0;

std::string error_file_loading_msg;


std::vector<float> buffer(BUFFER_SIZE);  

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

    // Tampons pour la FFT courante
    std::vector<float> currentLeft = std::vector<float>(FFT_SIZE, 0.0f);
    std::vector<float> currentRight = std::vector<float>(FFT_SIZE, 0.0f);
    std::vector<std::complex<float>> fftLeft = std::vector<std::complex<float>>(FFT_SIZE); std::vector<std::complex<float>> fftRight = std::vector<std::complex<float>>(FFT_SIZE);

    size_t paneHeight = WINDOW_HEIGHT / 2;

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



std::vector<uint32_t> pixelBuffer;
    // Ajoute une nouvelle colonne de fréquences calculées et décale le reste de l'image
    void addFFTFrame(const std::vector<float> *magnitudes) {
        // 1. Décaler tous les pixels de la texture d'un pixel vers la gauche
        for (int y = 0; y < WINDOW_HEIGHT; ++y) {
            std::memmove(&pixelBuffer[y * WINDOW_WIDTH], &pixelBuffer[y * WINDOW_WIDTH + 1], (WINDOW_WIDTH - 1) * sizeof(uint32_t));
        }

        // 2. Dessiner la nouvelle colonne tout à droite (X = WINDOW_WIDTH - 1)
        // L'axe Y représente les fréquences (Basses en bas, Hautes en haut)
        for (int y = 0; y < WINDOW_HEIGHT; ++y) {
            // Mapper la hauteur de l'écran sur la moitié utile de la FFT (frequences positives)
            uint32_t fftBin = (WINDOW_HEIGHT - 1 - y) * (FFT_SIZE / 2) / WINDOW_HEIGHT;

            size_t index = std::clamp(fftBin, (uint32_t)0, FFT_SIZE / 2 - 1);
            float mag = (*magnitudes)[index];

            // Normalisation de l'intensité lumineuse (application d'une échelle logarithmique)
            float intensity = std::clamp(20.0f * std::log10(mag + 1.0f) * 10.0f, 0.0f, 255.0f);
            uint8_t colorVal = static_cast<uint8_t>(intensity);

            // Génération d'une palette de couleur (Ex: Dégradé de Vert)
            uint32_t rgbaColor = (0x00 << 24) | (colorVal << 16) | (0x00 << 8) | 0xFF; // RGBA
            
            pixelBuffer[y * WINDOW_WIDTH + (WINDOW_WIDTH - 1)] = rgbaColor;
        }
        
    }

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

        std::thread ([](std::string file,std::vector<float> *samples){

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

            bool success = false;
            switch(status)
            {
            case 0:
            {
                return;
            }
            break;
            case 1:
                success = load_wav_file(file, samples,&c, &s);
                break;
            case 2:
                success = load_flac_file(file, samples,&c, &s);
                break;
            case 3:
                success = load_mp3_file(file, samples,&c, &s);
            }

            if ( success )
            {
                audio_file_data_t data = 
                {
                    .path = file,
                    .duration = ( static_cast<float>(samples->size())/static_cast<float>(c)/static_cast<float>(s) ),
                    .title = path.filename().string(),
                    .channels = c,
                    .sample_rate = s
                };
                data.samples.resize(samples->size());
                memcpy(data.samples.data(), samples->data(), samples->size() * sizeof(float));

                songs.emplace(hash, std::move(data) );

                channels.store(c);
                sample_rate.store(s);
                file_loaded.store(true );
            }
            file_loading.store(false);
            error_file_loading.store(!success);

        }, path, samples).detach();
    };

bool ImGuiStopButton(const char* str_id, ImVec2 size) {
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

// Paramètres de recherche pour l'autocorrélation
// Adapté pour capturer des fréquences de 50 Hz à 2000 Hz à 44.1 kHz
const int MIN_LAG = 22;   // ~2000 Hz (44100 / 22)
const int MAX_LAG = 882;  // ~50 Hz  (44100 / 882)

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

inline void  GetAutocorrectedSamples() 
{

    for (size_t k = 0; k < buffer.size()/2;k++)
    {
        rawLeft[k] = buffer[2 * k ], rawRight[k] = buffer[2 * k + 1];
    }

    const int MIN_LAG = 22;
    const int MAX_LAG = 882;


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


int main(int argc, char* argv[]) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        printf("Erreur d'initialisation SDL: %s\n", SDL_GetError());
        return -1;
    }

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

    spectrogram_renderer spectrogram(renderer,texture);

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
        };

    bool running = true;


    // Index de l'élément actuellement sélectionné (à déclarer en variable persistante/globale)
    static int current_window_idx = 0; 
    size_t id_selectionne = -1;
    size_t id_a_supprimer = -1;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_playing_time;

    double total_playing_time;

    float gain_value = -20.;

    std::vector<float> drawSamples  = std::vector<float>(DISPLAY_SAMPLES,0.) ;

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
                //if (closed_id == SDL_GetWindowID(window_ui)) {
                //    SDL_HideWindow(window_ui); // Masquer simplement l'UI si on clique sur sa croix
               // }
            }
        }


        if (playback.load() == true && SDL_GetAudioStreamQueued(playback_stream) < minimum_audio)
        {
            // this will feed 1024 samples each frame until we get to our maximum. 
            // generate samples from grooves 
            next_cursor.store( std::min(samples.size() - 1, samples_cursor.load() + buffer.size() ) );
            memcpy(buffer.data(), (const void *)(samples.data()+samples_cursor), (next_cursor - samples_cursor) * sizeof(float) );

            // feed the new data to the stream. It will queue at the end, and trickle out as the hardware needs more data. 
            SDL_PutAudioStreamData(playback_stream, buffer.data(), (next_cursor - samples_cursor) * sizeof(float) );
            samples_cursor.store( next_cursor );
            current_playing_time = samples_cursor/sample_rate/channels;
            if ( samples_cursor >= samples.size() - 1 )
            {
                playback.store(false);
            }

        }

        switch(current_window_idx)
        {
            case 0:
            {
                GetTriggeredSamples(0.0f, true);
            }
            break;
            case 1:
            {

                GetAutocorrectedSamples();
            }
            break;
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

        // Dessin de la forme d'onde (Lignes continues)
        SDL_SetRenderDrawColor(renderer, 0, 255, 128, 255);
        float hWidth = 800.0f / (DISPLAY_SAMPLES - 1);
        float centerY = 300.0f;
        float scale = 200.0f; // Hauteur de l'onde à l'écran

        float centerY_Left = 150.0f;
for (int i = 0; i < DISPLAY_SAMPLES - 1; ++i) {
    SDL_RenderLine(renderer, i * hWidth, centerY_Left - (output_left[i] * scale), 
                             (i + 1) * hWidth, centerY_Left - (output_left[i + 1] * scale));
}

// --- CANAL DROIT (Moitié Inférieure : Centre Y = 450) ---
SDL_SetRenderDrawColor(renderer, 255, 128, 0, 255); // Orange pour la Droite
float centerY_Right = 450.0f;
for (int i = 0; i < DISPLAY_SAMPLES - 1; ++i) 
{
    SDL_RenderLine(renderer, i * hWidth, centerY_Right - (output_right[i] * scale), 
                             (i + 1) * hWidth, centerY_Right - (output_right[i + 1] * scale));
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

        ImGui::Combo("oscilloscope visualizer", &current_window_idx, oscilloscope_visualizer_style, IM_ARRAYSIZE(oscilloscope_visualizer_style));
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Separator();
        if( ImGui::SliderFloat("Gain (dB)", &gain_value, -90, 0.) )
        {
            gain_db.store(gain_value);
            gain.store( std::pow( 10.f,gain_db/20.f ) );

        }
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Separator();
        
        // if capture to playback not selected
        if (current_window_idx != 2 && playback.load() == false )
        {
            ImGui::Text("choose audio file :");
            ImGui::TextUnformatted(selected_file_path.c_str());
            // 2. Bouton pour ouvrir l'explorateur
            if (ImGui::Button("open audio file...")) {
                // Configurer le titre et les extensions autorisées
                fileDialog.SetTitle("Choisir un fichier audio (.wav, .flac, .mp3)");
                fileDialog.SetTypeFilters({ ".wav", ".mp3", ".flac" });
                
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

                        if ( file_loaded.load() == true)
                        {
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
                            }
                        }
                        SDL_ResumeAudioStreamDevice(playback_stream);
                        
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
        if ( file_loaded.load() == true || current_window_idx == 2)
        {
            ImGui::Separator();
            if ( file_loaded && current_window_idx != 2 )
            {
                if ( playback.load() == false)
                {
                    if ( update_playback_stream.load() == true)
                    {
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
                            update_playback_stream.store(false);
                        }

                    }
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
            }
            ImGui::Spacing(); 
            ImGui::Dummy(ImVec2(0.0f, 25.0f)); 
            ImGui::Separator();
            // Bouton Play

            if (playback.load() == false && ImGuiPlayButton("##PlayBtn", ImVec2(32.0f, 32.0f)) ) 
            {
                play_position = 0;
                last_audio_cursor_stream = 0;
                audio_cursor_stream = 0;
                samples_cursor = 0;
                next_cursor = 0;
                SDL_ResumeAudioStreamDevice(playback_stream);
                if ( current_window_idx == 2)
                {
                    SDL_ResumeAudioStreamDevice(capture_stream);
                }
                
                playback.store(true);
                total_playing_time = samples.size()/channels / sample_rate;
                start_playing_time = now();
                
            }

            if (playback.load() == false)
            {
                ImGui::SameLine();
                if (ImGuiStopButton("##AudioStop", ImVec2(32.0f, 32.0f))) 
                {
                    last_audio_cursor_stream = 0;
                    audio_cursor_stream = 0;
                    samples_cursor = 0;
                    next_cursor = 0;
                    SDL_PauseAudioStreamDevice(playback_stream);
                    SDL_ClearAudioStream(playback_stream); 
                    if ( current_window_idx == 2)
                    {
                        SDL_PauseAudioStreamDevice(capture_stream);
                        SDL_ClearAudioStream(capture_stream); 
                    }
                    playback.store(false);
                }
            }
            else
            if ( ImGuiStopButton("##AudioStop", ImVec2(32.0f, 32.0f))) 
            {
                last_audio_cursor_stream = 0;
                audio_cursor_stream = 0;
                samples_cursor = 0;
                next_cursor = 0;
                SDL_PauseAudioStreamDevice(playback_stream);
                SDL_ClearAudioStream(playback_stream); 
                if ( current_window_idx == 2)
                {
                    SDL_PauseAudioStreamDevice(capture_stream);
                    SDL_ClearAudioStream(capture_stream); 
                }
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
                if (id_selectionne == id) {
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
                    id_selectionne = id;
                }

                // --- DEBUT DU MENU CONTEXTUEL (CLIC DROIT) ---
                // Cette fonction s'associe automatiquement au dernier TreeNodeEx affiché ci-dessus
                if (ImGui::BeginPopupContextItem()) 
                {
                    // Option de sélection rapide au clic droit
                    if (ImGui::MenuItem("Sélectionner")) {
                        id_selectionne = id;
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
                if (id_selectionne == id_a_supprimer) {
                    id_selectionne = 0; 
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