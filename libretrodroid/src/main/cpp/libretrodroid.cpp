#include <jni.h>

#include <EGL/egl.h>

#include <string>
#include <utility>
#include <vector>
#include <unordered_set>

#include "libretrodroid.h"
#include "utils/libretrodroidexception.h"
#include "log.h"
#include "core.h"
#include "audio.h"
#include "video.h"
#include "renderers/renderer.h"
#include "fpssync.h"
#include "input.h"
#include "rumble.h"
#include "shadermanager.h"
#include "utils/javautils.h"
#include "environment.h"
#include "renderers/es3/framebufferrenderer.h"
#include "renderers/es2/imagerendereres2.h"
#include "renderers/es3/imagerendereres3.h"
#include "utils/utils.h"
#include "utils/rect.h"
#include "errorcodes.h"
#include "vfs/vfs.h"

namespace libretrodroid {

uintptr_t LibretroDroid::callback_get_current_framebuffer() {
    return LibretroDroid::getInstance().handleGetCurrentFrameBuffer();
}

void LibretroDroid::callback_hw_video_refresh(
    const void *data,
    unsigned width,
    unsigned height,
    size_t pitch
) {
    LOGD("hw video refresh callback called %i %i", width, height);
    LibretroDroid::getInstance().handleVideoRefresh(data, width, height, pitch);
}

void LibretroDroid::callback_audio_sample(int16_t left, int16_t right) {
    LOGV("callback_audio_sample called (single sample path — not batched)");
}

size_t LibretroDroid::callback_set_audio_sample_batch(const int16_t *data, size_t frames) {
    return LibretroDroid::getInstance().handleAudioCallback(data, frames);
}

void LibretroDroid::callback_retro_set_input_poll() {}

int16_t LibretroDroid::callback_set_input_state(
    unsigned int port,
    unsigned int device,
    unsigned int index,
    unsigned int id
) {
    return LibretroDroid::getInstance().handleSetInputState(port, device, index, id);
}

void LibretroDroid::updateAudioSampleRateMultiplier() {
    if (audio) {
        audio->setPlaybackSpeed(frameSpeed);
    }
}

void LibretroDroid::resetGlobalVariables() {
    core = nullptr;
    audio = nullptr;
    video = nullptr;
    fpsSync = nullptr;
    input = nullptr;
    rumble = nullptr;
}

int LibretroDroid::availableDisks() {
    auto* cb = Environment::getInstance().getRetroDiskControlCallback();
    return cb ? static_cast<int>(cb->get_num_images()) : 0;
}

int LibretroDroid::currentDisk() {
    auto* cb = Environment::getInstance().getRetroDiskControlCallback();
    return cb ? static_cast<int>(cb->get_image_index()) : 0;
}

void LibretroDroid::changeDisk(unsigned int index) {
    auto* cb = Environment::getInstance().getRetroDiskControlCallback();
    if (!cb) {
        LOGE("Cannot swap disk: disk control callback not available.");
        return;
    }
    if (index >= cb->get_num_images()) {
        LOGE("Requested disk index %u is out of range.", index);
        return;
    }
    if (cb->get_image_index() != index) {
        cb->set_eject_state(true);
        cb->set_image_index(index);
        cb->set_eject_state(false);
    }
}

void LibretroDroid::updateVariable(const Variable& variable) {
    Environment::getInstance().updateVariable(variable.key, variable.value);
}

std::vector<Variable> LibretroDroid::getVariables() {
    return Environment::getInstance().getVariables();
}

std::vector<std::vector<struct Controller>> LibretroDroid::getControllers() {
    return Environment::getInstance().getControllers();
}

void LibretroDroid::setControllerType(unsigned int port, unsigned int type) {
    core->retro_set_controller_port_device(port, type);
}

bool LibretroDroid::unserializeState(const int8_t *data, size_t size) {
    std::lock_guard<std::mutex> lock(coreLock);
    return core->retro_unserialize(data, size);
}

bool LibretroDroid::unserializeSRAM(const int8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(coreLock);

    size_t sramSize = core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    void* sramState = core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);

    if (!sramState) {
        LOGE("Cannot load SRAM: nullptr from retro_get_memory_data");
        return false;
    }
    if (size > sramSize) {
        LOGE("Cannot load SRAM: provided size %zu > sram size %zu", size, sramSize);
        return false;
    }
    memcpy(sramState, data, size);
    return true;
}

std::vector<int8_t> LibretroDroid::serializeSRAM() {
    std::lock_guard<std::mutex> lock(coreLock);
    size_t size = core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    const auto* src = static_cast<const int8_t*>(core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    return std::vector<int8_t>(src, src + size);
}

void LibretroDroid::onSurfaceChanged(unsigned int width, unsigned int height) {
    LOGD("Performing libretrodroid onSurfaceChanged");
    video->updateScreenSize(width, height);
}

void LibretroDroid::onSurfaceCreated() {
    LOGD("Performing libretrodroid onSurfaceCreated");

    struct retro_system_av_info system_av_info {};
    core->retro_get_system_av_info(&system_av_info);

    video = nullptr;

    Video::RenderingOptions renderingOptions {
        Environment::getInstance().isUseHwAcceleration(),
        system_av_info.geometry.base_width,
        system_av_info.geometry.base_height,
        Environment::getInstance().isUseDepth(),
        Environment::getInstance().isUseStencil(),
        openglESVersion,
        Environment::getInstance().getPixelFormat()
    };

    video = std::make_unique<Video>(
        renderingOptions,
        fragmentShaderConfig,
        Environment::getInstance().isBottomLeftOrigin(),
        Environment::getInstance().getScreenRotation(),
        skipDuplicateFrames,
        immersiveModeEnabled,
        viewportRect,
        immersiveModeConfig
    );

    if (Environment::getInstance().getHwContextReset()) {
        Environment::getInstance().getHwContextReset()();
    }
}

void LibretroDroid::onMotionEvent(
    unsigned int port,
    unsigned int source,
    float xAxis,
    float yAxis
) {
    LOGD("Received motion event: %d %.2f, %.2f", source, xAxis, yAxis);
    if (input) {
        input->onMotionEvent(port, source, xAxis, yAxis);
    }
}

void LibretroDroid::onTouchEvent(float xAxis, float yAxis) {
    LOGD("Received touch event: %.2f, %.2f", xAxis, yAxis);
    if (input && video) {
        auto [x, y] = video->getLayout().getRelativePosition(xAxis, yAxis);
        input->onMotionEvent(0, Input::MOTION_SOURCE_POINTER, x, y);
    }
}

void LibretroDroid::onKeyEvent(unsigned int port, int action, int keyCode) {
    LOGD("Received key event with action (%d) and keycode (%d)", action, keyCode);
    if (input) {
        input->onKeyEvent(port, action, keyCode);
    }
}

void LibretroDroid::create(
    unsigned int GLESVersion,
    const std::string& soFilePath,
    const std::string& systemDir,
    const std::string& savesDir,
    std::vector<Variable> variables,
    const ShaderManager::Config& shaderConfig,
    float refreshRate,
    bool lowLatencyAudio,
    bool enableVirtualFileSystem,
    bool enableMicrophone,
    bool duplicateFrames,
    std::optional<ImmersiveMode::Config> immersiveModeConfig,
    const std::string& language
) {
    LOGD("Performing libretrodroid create");

    resetGlobalVariables();

    Environment::getInstance().initialize(systemDir, savesDir, &callback_get_current_framebuffer);
    Environment::getInstance().setLanguage(language);
    Environment::getInstance().setEnableVirtualFileSystem(enableVirtualFileSystem);
    Environment::getInstance().setEnableMicrophone(enableMicrophone);

    openglESVersion = GLESVersion;
    screenRefreshRate = refreshRate;
    skipDuplicateFrames = duplicateFrames;
    immersiveModeEnabled = (GLESVersion >= 3) && immersiveModeConfig.has_value();
    this->immersiveModeConfig = immersiveModeConfig.value_or(ImmersiveMode::Config{});
    audioEnabled = true;
    frameSpeed = 1;

    core = std::make_unique<Core>(soFilePath);

    core->retro_set_video_refresh(&callback_hw_video_refresh);
    core->retro_set_environment(&Environment::callback_environment);
    core->retro_set_audio_sample(&callback_audio_sample);
    core->retro_set_audio_sample_batch(&callback_set_audio_sample_batch);
    core->retro_set_input_poll(&callback_retro_set_input_poll);
    core->retro_set_input_state(&callback_set_input_state);

    for (const auto& v : variables) {
        updateVariable(v);
    }

    core->retro_init();

    preferLowLatencyAudio = lowLatencyAudio;

    if (Environment::getInstance().isUseHwAcceleration() && openglESVersion < 3) {
        throw LibretroDroidError("OpenGL ES 3 is required for this Core", ERROR_GL_NOT_COMPATIBLE);
    }

    fragmentShaderConfig = shaderConfig;
    rumble = std::make_unique<Rumble>();
}

void LibretroDroid::loadGameFromPath(const std::string& gamePath) {
    LOGD("Performing libretrodroid loadGameFromPath");

    struct retro_system_info system_info {};
    core->retro_get_system_info(&system_info);

    std::vector<char> fileData;
    struct retro_game_info game_info {};
    game_info.path = gamePath.c_str();
    game_info.meta = nullptr;

    if (system_info.need_fullpath) {
        game_info.data = nullptr;
        game_info.size = 0;
    } else {
        fileData = Utils::readFileAsBytes(gamePath);
        game_info.data = fileData.data();
        game_info.size = fileData.size();
    }

    if (!core->retro_load_game(&game_info)) {
        LOGE("Cannot load game from path.");
        throw std::runtime_error("Cannot load game");
    }

    afterGameLoad();
}

void LibretroDroid::loadGameFromBytes(const int8_t *data, size_t size) {
    LOGD("Performing libretrodroid loadGameFromBytes");

    struct retro_system_info system_info {};
    core->retro_get_system_info(&system_info);

    struct retro_game_info game_info {};
    game_info.path = nullptr;
    game_info.meta = nullptr;

    if (system_info.need_fullpath) {
        game_info.data = nullptr;
        game_info.size = 0;
    } else {
        game_info.data = data;
        game_info.size = size;
    }

    if (!core->retro_load_game(&game_info)) {
        LOGE("Cannot load game from bytes.");
        throw std::runtime_error("Cannot load game");
    }

    afterGameLoad();
}

void LibretroDroid::loadGameFromVirtualFiles(std::vector<VFSFile> virtualFiles) {
    LOGD("Performing libretrodroid loadGameFromVirtualFiles");

    if (virtualFiles.empty()) {
        LOGE("loadGameFromVirtualFiles called with no files.");
        throw std::runtime_error("No virtual files provided.");
    }

    struct retro_system_info system_info {};
    core->retro_get_system_info(&system_info);

    const std::string firstFilePath = virtualFiles[0].getFileName();
    int firstFileFD = virtualFiles[0].getFD();
    bool loadUsingVFS = system_info.need_fullpath || virtualFiles.size() > 1;

    if (loadUsingVFS) {
        VFS::getInstance().initialize(std::move(virtualFiles));
    }

    std::vector<char> fileData;
    struct retro_game_info game_info {};
    game_info.path = firstFilePath.c_str();
    game_info.meta = nullptr;

    if (loadUsingVFS) {
        game_info.data = nullptr;
        game_info.size = 0;
    } else {
        fileData = Utils::readFileAsBytes(firstFileFD);
        game_info.data = fileData.data();
        game_info.size = fileData.size();
    }

    if (!core->retro_load_game(&game_info)) {
        LOGE("Cannot load game from virtual files.");
        throw std::runtime_error("Cannot load game");
    }

    afterGameLoad();
}

void LibretroDroid::destroy() {
    std::lock_guard<std::mutex> lock(coreLock);

    LOGD("Performing libretrodroid destroy");

    if (Environment::getInstance().getHwContextDestroy()) {
        Environment::getInstance().getHwContextDestroy()();
    }

    core->retro_unload_game();
    core->retro_deinit();

    video = nullptr;
    core = nullptr;
    rumble = nullptr;
    fpsSync = nullptr;
    audio = nullptr;

    Environment::getInstance().deinitialize();
    VFS::getInstance().deinitialize();
}

void LibretroDroid::resume() {
    LOGD("Performing libretrodroid resume");
    input = std::make_unique<Input>();
    fpsSync->reset();
    audio->start();
    refreshAspectRatio();
}

void LibretroDroid::pause() {
    LOGD("Performing libretrodroid pause");
    audio->stop();
    input = nullptr;
}

void LibretroDroid::step() {
    std::lock_guard<std::mutex> lock(coreLock);

    LOGD("Stepping into retro_run()");

    unsigned frames = 1;
    if (__builtin_expect(fpsSync != nullptr, 1)) {
        frames = fpsSync->advanceFrames();
    }

    const unsigned totalRuns = frames * frameSpeed;
    for (unsigned i = 0; __builtin_expect(i < totalRuns, 1); ++i) {
        core->retro_run();
    }

    if (__builtin_expect(video != nullptr && !video->rendersInVideoCallback(), 1)) {
        video->renderFrame();
    }

    if (__builtin_expect(fpsSync != nullptr, 1)) {
        fpsSync->wait();
    }

    if (__builtin_expect(rumble != nullptr && rumbleEnabled, 0)) {
        rumble->fetchFromEnvironment();
    }

    if (video && Environment::getInstance().isGameGeometryUpdated()) {
        Environment::getInstance().clearGameGeometryUpdated();
        video->updateRendererSize(
            Environment::getInstance().getGameGeometryWidth(),
            Environment::getInstance().getGameGeometryHeight()
        );
        dirtyVideo = true;
    }

    if (Environment::getInstance().isGameTimingUpdated()) {
        double newFps        = Environment::getInstance().getGameTimingFps();
        double newSampleRate = Environment::getInstance().getGameTimingSampleRate();
        Environment::getInstance().clearGameTimingUpdated();

        LOGI("Core updated AV timing: fps=%.3f sampleRate=%.1f — recreating fpsSync and audio",
             newFps, newSampleRate);

        fpsSync = std::make_unique<FPSSync>(newFps, screenRefreshRate);
        double inputSampleRate = newSampleRate * fpsSync->getTimeStretchFactor();
        audio = std::make_unique<Audio>(
            static_cast<int32_t>(std::lround(inputSampleRate)),
            newFps,
            preferLowLatencyAudio
        );
        updateAudioSampleRateMultiplier();
        audio->start();
    }

    if (video && Environment::getInstance().isScreenRotationUpdated()) {
        Environment::getInstance().clearScreenRotationUpdated();
        video->updateRotation(Environment::getInstance().getScreenRotation());
    }
}

float LibretroDroid::getAspectRatio() {
    float gameAspectRatio = Environment::getInstance().retrieveGameSpecificAspectRatio();
    return gameAspectRatio > 0 ? gameAspectRatio : defaultAspectRatio;
}

void LibretroDroid::refreshAspectRatio() {
    float effective = (aspectRatioOverride > 0.0f) ? aspectRatioOverride : getAspectRatio();
    video->updateAspectRatio(effective);
}

void LibretroDroid::setAspectRatioOverride(float ratio) {
    aspectRatioOverride = ratio;
    float effective = (ratio > 0.0f) ? ratio : getAspectRatio();
    video->updateAspectRatio(effective);
}

void LibretroDroid::setRumbleEnabled(bool enabled) { rumbleEnabled = enabled; }
bool LibretroDroid::isRumbleEnabled() const { return rumbleEnabled; }

void LibretroDroid::setFrameSpeed(unsigned int speed) {
    frameSpeed = speed;
    updateAudioSampleRateMultiplier();
}

void LibretroDroid::setSkipDuplicateFrames(bool skip) { skipDuplicateFrames = skip; }
void LibretroDroid::setAudioEnabled(bool enabled) { audioEnabled = enabled; }

void LibretroDroid::setShaderConfig(ShaderManager::Config shaderConfig) {
    fragmentShaderConfig = std::move(shaderConfig);
    if (video) {
        video->updateShaderType(fragmentShaderConfig);
    }
}

void LibretroDroid::handleVideoRefresh(
    const void *data,
    unsigned int width,
    unsigned int height,
    size_t pitch
) {
    if (video) {
        video->onNewFrame(data, width, height, pitch);
        if (video->rendersInVideoCallback()) {
            video->renderFrame();
        }
    }
}

size_t LibretroDroid::handleAudioCallback(const int16_t *data, size_t frames) {
    if (audio && audioEnabled) {
        audio->write(data, frames);
    }
    return frames;
}

int16_t LibretroDroid::handleSetInputState(
    unsigned int port,
    unsigned int device,
    unsigned int index,
    unsigned int id
) {
    return input ? input->getInputState(port, device, index, id) : 0;
}

uintptr_t LibretroDroid::handleGetCurrentFrameBuffer() {
    return video ? video->getCurrentFramebuffer() : 0;
}

void LibretroDroid::reset() {
    std::lock_guard<std::mutex> lock(coreLock);
    core->retro_reset();
}

std::vector<int8_t> LibretroDroid::serializeState() {
    std::lock_guard<std::mutex> lock(coreLock);
    size_t size = core->retro_serialize_size();
    std::vector<int8_t> data(size);
    core->retro_serialize(data.data(), size);
    return data;
}

void LibretroDroid::resetCheat() {
    std::lock_guard<std::mutex> lock(coreLock);
    core->retro_cheat_reset();
}

void LibretroDroid::setCheat(unsigned index, bool enabled, const std::string& code) {
    std::lock_guard<std::mutex> lock(coreLock);
    core->retro_cheat_set(index, enabled, code.c_str());
}

bool LibretroDroid::requiresVideoRefresh() const { return dirtyVideo; }
void LibretroDroid::clearRequiresVideoRefresh() { dirtyVideo = false; }

void LibretroDroid::afterGameLoad() {
    struct retro_system_av_info system_av_info {};
    core->retro_get_system_av_info(&system_av_info);

    fpsSync = std::make_unique<FPSSync>(system_av_info.timing.fps, screenRefreshRate);

    double inputSampleRate = system_av_info.timing.sample_rate * fpsSync->getTimeStretchFactor();
    audio = std::make_unique<Audio>(
        static_cast<int32_t>(std::lround(inputSampleRate)),
        system_av_info.timing.fps,
        preferLowLatencyAudio
    );
    updateAudioSampleRateMultiplier();

    videoBaseWidth  = system_av_info.geometry.base_width  > 0 ? system_av_info.geometry.base_width  : 1;
    videoBaseHeight = system_av_info.geometry.base_height > 0 ? system_av_info.geometry.base_height : 1;
    defaultAspectRatio = findDefaultAspectRatio(system_av_info);
}

float LibretroDroid::findDefaultAspectRatio(const retro_system_av_info& system_av_info) {
    float result = system_av_info.geometry.aspect_ratio;
    if (result < 0) {
        result = static_cast<float>(system_av_info.geometry.base_width) /
                 static_cast<float>(system_av_info.geometry.base_height);
    }
    return result;
}

void LibretroDroid::handleRumbleUpdates(const std::function<void(int, float, float)>& handler) {
    if (rumble && rumbleEnabled) {
        rumble->handleRumbleUpdates(handler);
    }
}

void LibretroDroid::setViewport(Rect viewportRect) {
    this->viewportRect = viewportRect;
    if (video) {
        video->updateViewportSize(viewportRect);
    }
}

unsigned int LibretroDroid::getVideoBaseWidth() const { return videoBaseWidth; }
unsigned int LibretroDroid::getVideoBaseHeight() const { return videoBaseHeight; }

} //namespace libretrodroid
