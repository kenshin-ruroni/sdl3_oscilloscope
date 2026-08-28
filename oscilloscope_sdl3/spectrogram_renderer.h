#include <SDL3/SDL.h>
#include <vector>
#include <cmath>
#include <complex>
#include <algorithm>
#include <cstring>

constexpr uint32_t FFT_SIZE = 1024;
constexpr  uint32_t WINDOW_WIDTH = 800;
constexpr  uint32_t WINDOW_HEIGHT = 800;



// Gestionnaire du Spectrogramme
class spectrogram_renderer {
public:
    spectrogram_renderer(SDL_Renderer* renderer, SDL_Texture*  texture) : m_renderer(renderer),m_texture(texture) {
        // Création d'une texture en mode "Streaming" pour pouvoir modifier ses pixels à la volée
        
        m_pixelBuffer.resize(WINDOW_WIDTH * WINDOW_HEIGHT, 0x000000FF); // Remplir de noir opaque
        g_audioMutex = SDL_CreateMutex();
        SDL_UpdateTexture(m_texture, nullptr, m_pixelBuffer.data(), WINDOW_WIDTH * sizeof(uint32_t));
    }

    ~spectrogram_renderer() {

    }

    SDL_Texture*  m_texture;

    std::vector<uint32_t> m_pixelBuffer;
    // Ajoute une nouvelle colonne de fréquences calculées et décale le reste de l'image
    void addFFTFrame(const std::vector<float> *magnitudes) {
        // 1. Décaler tous les pixels de la texture d'un pixel vers la gauche
        for (int y = 0; y < WINDOW_HEIGHT; ++y) {
            std::memmove(&m_pixelBuffer[y * WINDOW_WIDTH], &m_pixelBuffer[y * WINDOW_WIDTH + 1], (WINDOW_WIDTH - 1) * sizeof(uint32_t));
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
            
            m_pixelBuffer[y * WINDOW_WIDTH + (WINDOW_WIDTH - 1)] = rgbaColor;
        }
        
    }
    // Tampons pour la FFT courante
    std::vector<float> currentLeft = std::vector<float>(FFT_SIZE, 0.0f);
    std::vector<float> currentRight = std::vector<float>(FFT_SIZE, 0.0f);
    std::vector<std::complex<float>> fftLeft = std::vector<std::complex<float>>(FFT_SIZE); std::vector<std::complex<float>> fftRight = std::vector<std::complex<float>>(FFT_SIZE);

    size_t paneHeight = WINDOW_HEIGHT / 2;

    inline void update_frame(float *audio_buffer, size_t audio_buffer_size)
    {

        for (size_t  i = 0; i < FFT_SIZE; ++i) 
        {
            currentLeft[i] = audio_buffer[i * 2];
            currentRight[i] = audio_buffer[i * 2 + 1];
        }

        for (int i = 0; i < FFT_SIZE; ++i) {
            float windowMultiplier = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (FFT_SIZE - 1)));
            fftLeft[i] = std::complex<float>(currentLeft[i] * windowMultiplier, 0.0f);
            fftRight[i] = std::complex<float>(currentRight[i] * windowMultiplier, 0.0f);
        }
        fft(&fftLeft);
        fft(&fftRight);

        int usableBins = FFT_SIZE / 2; // Fréquences utiles uniques
        float barWidth = static_cast<float>(WINDOW_WIDTH) / usableBins;
        for (int i = 0; i < usableBins; ++i) {
            // Calcul de l'amplitude (mise à l'échelle logarithmique visuelle)
            float magLeft = std::abs(fftLeft[i]) / std::sqrt(FFT_SIZE);
            float magRight = std::abs(fftRight[i]) / std::sqrt(FFT_SIZE);

            float normLeft = std::clamp(std::log1p(magLeft * 20.0f) / 3.0f, 0.0f, 1.0f);
            float normRight = std::clamp(std::log1p(magRight * 20.0f) / 3.0f, 0.0f, 1.0f);

            int x = static_cast<int>(i * barWidth);
            int w = std::max(1, static_cast<int>(barWidth));

            // --- Canal Gauche (Panneau du haut) ---
            int hLeft = static_cast<int>(normLeft * (paneHeight - 20));
            SDL_FRect rectLeft{ (float)x, (float)(paneHeight - hLeft), (float)w, (float)hLeft };
            SDL_SetRenderDrawColor(m_renderer, 0, 180, 255, 255); // Cyan
            SDL_RenderFillRect(m_renderer, &rectLeft);

            // --- Canal Droit (Panneau du bas) ---
            int hRight = static_cast<int>(normRight * (paneHeight - 20));
            SDL_FRect rectRight{ (float)x, (float)(WINDOW_HEIGHT - hRight), (float)w, (float)hRight };
            SDL_SetRenderDrawColor(m_renderer, 255, 0, 128, 255); // Magenta
            SDL_RenderFillRect(m_renderer, &rectRight);
        }

    }

    void render() {
        SDL_RenderTexture(m_renderer, m_texture, nullptr, nullptr);
    }

// Fonction FFT Cooley-Tukey (identique)
void fft(std::vector<std::complex<float>>* a) {
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

std::vector<float> g_latestMagnitudes = std::vector<float> (FFT_SIZE/2, 0.0f);

SDL_Mutex* g_audioMutex = nullptr;

// Callback de lecture de SDL3
static void SDLCALL AudioAnalysisCallback(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount) {
    
    spectrogram_renderer *renderer = (spectrogram_renderer *)userdata;
    int samplesNeeded = additional_amount / sizeof(float);
    if (samplesNeeded < FFT_SIZE) return;

    std::vector<float> audioSamples(FFT_SIZE, 0.0f);
    // Récupérer les données du flux
    SDL_GetAudioStreamData(stream, audioSamples.data(), FFT_SIZE * sizeof(float));

    // Préparer les données pour la FFT complexe
    std::vector<std::complex<float>> fftData(FFT_SIZE);
    for (int i = 0; i < FFT_SIZE; ++i) {
        // Optionnel mais recommandé : appliquer une fenêtre de Hamming ici pour éviter le "spectral leakage"
        fftData[i] = std::complex<float>(audioSamples[i], 0.0f);
    }

    renderer->fft(&fftData);

    // Extraction des amplitudes de la moitié positive du spectre
    SDL_LockMutex(renderer->g_audioMutex);
    for (int i = 0; i < FFT_SIZE / 2; ++i) {
        renderer->g_latestMagnitudes[i] = std::abs(fftData[i]);
    }
    SDL_UnlockMutex(renderer->g_audioMutex);
}

private:
    SDL_Renderer* m_renderer;


};