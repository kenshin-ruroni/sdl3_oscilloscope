#include <emmintrin.h> // En-tête obligatoire pour SSE2
#include <vector>
#include <cmath>
#include <complex>

// Alignement structurel requis pour l'allocation SSE2 sécurisée
struct alignas(16) ComplexSSE {
    float r; // Réel
    float i; // Imaginaire
};

class sse2_fft {
private:

    int log2fft_size;
    std::vector<int> bit_rev_table;
    
    // Tables Twiddle alignées à 16 octets pour les chargements SSE2 directs
    alignas(16) std::vector<float> twiddle_cos;
    alignas(16) std::vector<float> twiddle_sin;

    // Génère l'inversion des bits pour le réarrangement initial
    void precompute_bit_reversal() {
        bit_rev_table.resize(fft_size);
        for (int i = 0; i < fft_size; ++i) {
            int rev = 0;
            int temp = i;
            for (int j = 0; j < log2fft_size; ++j) {
                rev = (rev << 1) | (temp & 1);
                temp >>= 1;
            }
            bit_rev_table[i] = rev;
        }
    }

    // Précalcule les tables trigonométriques de manière ultra-précise
    void precompute_twiddle_factors() {
        twiddle_cos.resize(fft_size / 2);
        twiddle_sin.resize(fft_size / 2);
        for (int i = 0; i < fft_size / 2; ++i) {
            double angle = -2.0 * M_PI * i / fft_size;
            twiddle_cos[i] = static_cast<float>(std::cos(angle));
            twiddle_sin[i] = static_cast<float>(std::sin(angle));
        }
    }

    size_t fft_size;
    alignas(16) std::vector<ComplexSSE> input_buffer_left;
    alignas(16) std::vector<ComplexSSE> output_buffer_left;
    alignas(16) std::vector<ComplexSSE> input_buffer_right;
    alignas(16) std::vector<ComplexSSE> output_buffer_right;

public:
    sse2_fft(int size):fft_size(size) {
        log2fft_size = static_cast<int>(std::log2(fft_size));
        precompute_bit_reversal();
        precompute_twiddle_factors();

        input_buffer_left.resize(size);
        output_buffer_left.resize(size);
        input_buffer_right.resize(size);
        output_buffer_right.resize(size);    


    }

       // Input et Output doivent être alignés à 16 octets (ex: std::vector avec allocateur aligné ou alignas)
    void process(const ComplexSSE* input, ComplexSSE* output) {
        // Étape 1 : Réarrangement Bit-Reversal Robuste
        for (int i = 0; i < fft_size; ++i) {
            output[bit_rev_table[i]] = input[i];
        }

        // Étape 2 : Boucle des étages de la FFT
        for (int stage = 1; stage <= log2fft_size; ++stage) {
            int m = 1 << stage;      // Taille du sous-problème actuel
            int m2 = m >> 1;         // Moitié (m/2)
            int twiddle_step = fft_size / m;

            // Si la taille du bloc est trop petite, SSE2 n'est pas rentable (overhead).
            // On vectorise SSE2 uniquement dès que m2 >= 2 (soit au moins 2 nombres complexes)
            if (m2 >= 2) {
                for (int k = 0; k < fft_size; k += m) {
                    for (int j = 0; j < m2; j += 2) { // Progression par pas de 2 complexes (SSE2)
                        int idx1 = k + j;
                        int idx2 = idx1 + m2;

                        // Chargement des facteurs de rotation (Twiddle)
                        int tw_idx1 = j * twiddle_step;
                        int tw_idx2 = (j + 1) * twiddle_step;

                        // On prépare les registres SSE2 [Complexe 2 | Complexe 1]
                        __m128 wr = _mm_set_ps(twiddle_cos[tw_idx2], twiddle_cos[tw_idx2], twiddle_cos[tw_idx1], twiddle_cos[tw_idx1]);
                        __m128 wi = _mm_set_ps(twiddle_sin[tw_idx2], twiddle_sin[tw_idx2], twiddle_sin[tw_idx1], twiddle_sin[tw_idx1]);

                        // Chargement des données de la RAM vers les registres SSE2 (Lecture alignée obligatoire)
                        __m128 odd = _mm_load_ps(reinterpret_cast<float*>(&output[idx2])); 
                        __m128 even = _mm_load_ps(reinterpret_cast<float*>(&output[idx1]));

                        // Séparation des composantes Réelles et Imaginaires de 'odd' via Shuffling SSE
                        // odd_r = [odd[1].r, odd[1].r, odd[0].r, odd[0].r]
                        __m128 odd_r = _mm_shuffle_ps(odd, odd, _MM_SHUFFLE(2, 2, 0, 0));
                        // odd_i = [odd[1].i, odd[1].i, odd[0].i, odd[0].i]
                        __m128 odd_i = _mm_shuffle_ps(odd, odd, _MM_SHUFFLE(3, 3, 1, 1));

                        // Multiplication Complexe Vectorielle : (A+iB)*(C+iD) = (AC - BD) + i(AD + BC)
                        __m128 t1 = _mm_mul_ps(odd_r, wr); // AC
                        __m128 t2 = _mm_mul_ps(odd_i, wi); // BD
                        __m128 t3 = _mm_mul_ps(odd_r, wi); // AD
                        __m128 t4 = _mm_mul_ps(odd_i, wr); // BC

                        // Reconstruction de la partie réelle (AC - BD) et imaginaire (AD + BC)
                        __m128 r_part = _mm_sub_ps(t1, t2);
                        __m128 i_part = _mm_add_ps(t3, t4);

                        // Entrelacement pour reformer le type [Real, Imag, Real, Imag]
                        __m128 r_i_combined = _mm_shuffle_ps(r_part, i_part, _MM_SHUFFLE(1, 0, 1, 0));
                        // Re-shuffling final pour ordonner correctement les deux complexes dans le registre
                        __m128 t_odd = _mm_shuffle_ps(r_i_combined, r_i_combined, _MM_SHUFFLE(3, 1, 2, 0));

                        // Application des papillons de Cooley-Tukey (Addition et Soustraction simultanées)
                        __m128 res_even = _mm_add_ps(even, t_odd);
                        __m128 res_odd  = _mm_sub_ps(even, t_odd);

                        // Écriture alignée des résultats directement en RAM
                        _mm_store_ps(reinterpret_cast<float*>(&output[idx1]), res_even);
                        _mm_store_ps(reinterpret_cast<float*>(&output[idx2]), res_odd);
                    }
                }
            } else {
                // Étape de repli Scalaire standard (Uniquement pour le tout premier étage où m2 = 1)
                for (int k = 0; k < fft_size; k += m) {
                    for (int j = 0; j < m2; ++j) {
                        int idx1 = k + j;
                        int idx2 = idx1 + m2;
                        
                        float wr = twiddle_cos[j * twiddle_step];
                        float wi = twiddle_sin[j * twiddle_step];

                        ComplexSSE t_odd;
                        t_odd.r = output[idx2].r * wr - output[idx2].i * wi;
                        t_odd.i = output[idx2].r * wi + output[idx2].i * wr;

                        output[idx2].r = output[idx1].r - t_odd.r;
                        output[idx2].i = output[idx1].i - t_odd.i;
                        output[idx1].r += t_odd.r;
                        output[idx1].i += t_odd.i;
                    }
                }
            }
        }
    }

    void process_audio_spectrum(const std::vector<float> *audio_samples, std::vector<float> *magnitudes_left,  std::vector<float> *magnitudes_right) {

    // Allocation forcée sur des frontières de 16 octets pour éviter les plantages SSE2
   // alignas(16) ComplexSSE input_buffer_left[fft_size];
   // alignas(16) ComplexSSE output_buffer_left[fft_size];
   // alignas(16) ComplexSSE input_buffer_right[fft_size];
   // alignas(16) ComplexSSE output_buffer_right[fft_size];

    // Remplissage du buffer complexe (R = sample audio, I = 0.0f)
    for (int i = 0; i < fft_size; ++i) {
        input_buffer_left[i].r = (*audio_samples)[2 * i] ;
        input_buffer_left[i].i = 0.0f;
        input_buffer_right[i].r = (*audio_samples)[2 * i  + 1] ;
        input_buffer_right[i].i = 0.0f;
    }

    // Calcul de la FFT via SSE2 (Zéro allocation dynamique pendant le calcul)
    process(input_buffer_left.data(), output_buffer_left.data());
    process(input_buffer_right.data(), output_buffer_right.data());

    // Extraction des magnitudes pour les injecter dans votre visualiseur dB
    magnitudes_left->resize( fft_size / 2);
    magnitudes_right->resize( fft_size / 2 );
    for (int i = 0; i < fft_size / 2; ++i) 
    {
        float r = output_buffer_left[i].r;
        float i_img = output_buffer_left[i].i;
        // Calcul de la puissance linéaire
        (*magnitudes_left)[i] = std::sqrt(r * r + i_img * i_img) / fft_size;
        r = output_buffer_right[i].r;
        i_img = output_buffer_right[i].i;
        // Calcul de la puissance linéaire
        (*magnitudes_right)[i] = std::sqrt(r * r + i_img * i_img) / fft_size;
    }
}
};