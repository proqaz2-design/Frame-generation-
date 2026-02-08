# ⚡ FrameGen — AI Frame Generation for Android Games

> Реальна генерація кадрів для Android ігор без Root. Vulkan + NCNN + RIFE.

## 🏗 Архітектура

```
┌─────────────────────────────────────────────────────────────────┐
│                        Android App (Kotlin)                      │
│  ┌──────────┐  ┌──────────────┐  ┌──────────────────────────┐  │
│  │ Game      │  │ Refresh Rate │  │ Performance              │  │
│  │ Launcher  │  │ Controller   │  │ Monitor UI               │  │
│  └──────┬───┘  └──────┬───────┘  └──────────┬───────────────┘  │
│         │              │                      │                   │
│  ═══════╪══════════════╪══════════════════════╪═══════════════   │
│         │     JNI Bridge (framegen_jni.cpp)   │                   │
│  ═══════╪══════════════╪══════════════════════╪═══════════════   │
│         ▼              ▼                      ▼                   │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                   Native C++ Engine                       │   │
│  │                                                           │   │
│  │  ┌─────────────┐    ┌──────────────────┐                │   │
│  │  │ Vulkan Layer │───▶│ Frame Capture    │                │   │
│  │  │ (Intercept   │    │ (Ring Buffer)    │                │   │
│  │  │  Present)    │    └────────┬─────────┘                │   │
│  │  └─────────────┘             │                           │   │
│  │                    ┌─────────▼──────────┐                │   │
│  │                    │ Motion Estimation   │                │   │
│  │                    │ (Optical Flow GPU)  │                │   │
│  │                    └─────────┬──────────┘                │   │
│  │                    ┌─────────▼──────────┐                │   │
│  │                    │ RIFE Interpolation  │                │   │
│  │                    │ (NCNN / GPU Compute)│                │   │
│  │                    └─────────┬──────────┘                │   │
│  │                    ┌─────────▼──────────┐                │   │
│  │                    │ Frame Presenter     │                │   │
│  │                    │ (Precise Timing)    │                │   │
│  │                    └─────────┬──────────┘                │   │
│  │                              │                           │   │
│  │  ┌──────────────┐  ┌────────▼────────┐                  │   │
│  │  │ Timing       │  │ Thermal         │                  │   │
│  │  │ Controller   │  │ Protection      │                  │   │
│  │  └──────────────┘  └─────────────────┘                  │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
                    ┌─────────────────┐
                    │   Display 120Hz  │
                    │   (via Surface   │
                    │    Control API)  │
                    └─────────────────┘
```

## 🔧 Як це працює

### 1. Захоплення кадрів (Vulkan Layer)
- Vulkan implicit layer `VK_LAYER_FRAMEGEN_capture` перехоплює `vkQueuePresentKHR`
- Копіює кожен кадр гри в GPU ring-buffer (4 кадри)
- Весь процес відбувається на GPU — **нуль копіювань CPU↔GPU**

### 2. Аналіз руху (Motion Estimation)
- **Ієрархічний block matching** з 4-рівневою пірамідою зображень
- **Diamond search** для швидкого пошуку (замість повного перебору)
- **Sub-pixel refinement** через параболічну апроксимацію
- Forward-backward consistency check для виявлення оклюзій
- Все працює через **Vulkan compute shaders** на GPU

### 3. Інтерполяція (RIFE / Fallback)
- **Основний метод:** NCNN (Tencent) з моделлю RIFE v4.6 Lite
  - FP16 inference на Vulkan
  - Оптимізовано під мобільні GPU (Adreno, Mali, PowerVR)
- **Fallback:** GPU compute pipeline
  - Optical flow → Frame warp → Occlusion-aware blend
  - Працює на будь-якому Vulkan 1.1 GPU

### 4. Презентація кадрів
- Lock-free SPSC queue для zero-latency доставки
- Precise timing з busy-wait для останньої мілісекунди
- Адаптивна якість: автоматично зменшує роздільність моделі при перевищенні бюджету

### 5. Захист від перегріву
- Читає температуру GPU з `/sys/class/thermal/`
- При 75°C: зменшує якість
- При 85°C: fallback на мінімальну роздільність
- Асиметрична адаптація: швидкий downgrade, повільний upgrade

## 📁 Структура проекту

```
app/src/main/
├── cpp/                          # Native C++ (NDK)
│   ├── CMakeLists.txt            # Build configuration
│   ├── framegen_jni.cpp          # JNI bridge
│   ├── framegen_types.h          # Core types & config
│   ├── vulkan/                   # Vulkan Layer & GPU
│   │   ├── vulkan_layer.h/cpp    # Implicit Vulkan layer
│   │   ├── vulkan_capture.h/cpp  # Frame capture ring buffer
│   │   └── vulkan_compute.h/cpp  # Compute pipeline manager
│   ├── interpolation/            # Frame interpolation
│   │   ├── rife_engine.h/cpp     # RIFE neural network (NCNN)
│   │   ├── motion_estimator.h/cpp# Hierarchical block matching
│   │   └── optical_flow.h/cpp    # Bidirectional optical flow
│   ├── pipeline/                 # Frame delivery
│   │   ├── frame_queue.h/cpp     # Lock-free SPSC queue
│   │   ├── frame_presenter.h/cpp # Pipeline orchestrator
│   │   └── timing_controller.h/cpp # Adaptive quality
│   ├── utils/                    # Utilities
│   │   ├── gpu_buffer.h/cpp      # Vulkan buffer wrapper
│   │   ├── shader_compiler.h/cpp # SPIR-V loader
│   │   └── perf_monitor.h/cpp    # Performance tracking
│   └── shaders/                  # GLSL compute shaders
│       ├── optical_flow.comp     # Block matching SAD
│       ├── block_match.comp      # Diamond search
│       ├── frame_warp.comp       # Bilinear warp
│       ├── frame_blend.comp      # Occlusion-aware blend
│       ├── flow_refine.comp      # Edge-aware bilateral filter
│       ├── flow_consistency.comp # Forward-backward check
│       ├── downsample.comp       # Image pyramid
│       └── rgb_to_gray.comp      # Luma conversion
├── java/com/framegen/app/
│   ├── MainActivity.kt           # UI
│   └── engine/
│       ├── FrameGenEngine.kt     # Native engine wrapper
│       ├── RefreshRateController.kt # Display Hz control
│       └── GameLauncher.kt       # Game discovery & launch
├── res/layout/activity_main.xml  # UI layout
├── assets/VkLayer_framegen.json  # Vulkan layer manifest
└── AndroidManifest.xml
```

## 🛠 Збірка

### Вимоги
- Android Studio Arctic Fox або новіша
- Android NDK 26.1+ (встановлюється автоматично)
- Android SDK 34
- Пристрій з **Vulkan 1.1** та **ARM64**

### Кроки

1. **Клонувати репозиторій:**
   ```bash
   git clone https://github.com/your-username/Frame-generation-.git
   cd Frame-generation-
   ```

2. **Завантажити NCNN (опціонально, для AI моделі):**
   ```bash
   mkdir -p app/src/main/cpp/third_party
   cd app/src/main/cpp/third_party
   wget https://github.com/Tencent/ncnn/releases/download/20240102/ncnn-20240102-android-vulkan.zip
   unzip ncnn-20240102-android-vulkan.zip
   mv ncnn-20240102-android-vulkan ncnn-android-vulkan
   ```

3. **Завантажити RIFE модель (опціонально):**
   ```bash
   # Модель RIFE v4.6 Lite для NCNN:
   # Розмістити rife-v4.6-lite.param та .bin у:
   # /data/data/com.framegen.app/files/models/
   ```

4. **Компілювати шейдери (потрібен glslangValidator):**
   ```bash
   apt install glslang-tools
   cd app/src/main/cpp/shaders
   for f in *.comp; do
       glslangValidator -V "$f" -o "${f%.comp}.spv"
   done
   mkdir -p ../../assets/shaders
   cp *.spv ../../assets/shaders/
   ```

5. **Зібрати в Android Studio:**
   ```
   File → Open → вибрати кореневу папку проекту
   Build → Make Project
   ```

## 📱 Використання

### Без Shizuku (потрібен ADB):
```bash
adb shell settings put global enable_gpu_debug_layers 1
adb shell settings put global gpu_debug_app com.target.game
adb shell settings put global gpu_debug_layers VK_LAYER_FRAMEGEN_capture
adb shell settings put global gpu_debug_layer_app com.framegen.app
```

### З Shizuku (без комп'ютера):
1. Встановити [Shizuku](https://shizuku.rikka.app/) з Play Store
2. Активувати Shizuku через wireless debugging (Android 11+)
3. Відкрити FrameGen → вибрати гру → START

## ⚙ Режими роботи

| Режим | Вхід | Вихід | Інтерп. кадрів | Бюджет |
|-------|------|-------|-----------------|--------|
| OFF | — | — | 0 | — |
| 30→60 | 30 fps | 60 fps | 1 | 16.6 ms |
| 30→90 | 30 fps | 90 fps | 2 | 11.1 ms |
| 30→120 | 30 fps | 120 fps | 3 | 8.3 ms |

## 🔬 Технічні деталі

### Затримка (Input Lag)
- Захоплення кадру: **< 1 ms** (GPU copy)
- Motion estimation: **1-3 ms** (compute shader)
- Інтерполяція: **2-5 ms** (NCNN) або **3-6 ms** (fallback)
- Загальний overhead: **4-8 ms**

### Підтримувані GPU
- Qualcomm Adreno 600+ серія
- ARM Mali G76+
- PowerVR Series9+
- Samsung Xclipse

## ⚠ Обмеження

1. **Тільки ARM64** — x86 Android не підтримується
2. **Vulkan 1.1** мінімум — старі GPU не працюватимуть
3. **Без Root** — але потрібен ADB або Shizuku для Vulkan layer
4. **Нагрів** — thermal throttling автоматично зменшить якість
5. **Не всі ігри** — тільки Vulkan ігри (не OpenGL ES)

## 📜 Ліцензія

MIT License
