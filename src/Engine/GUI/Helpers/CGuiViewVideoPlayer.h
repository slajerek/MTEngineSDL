// CGuiViewVideoPlayer.h — Generic video player helper view (WebM/VP9/Opus)
#pragma once

#include "CGuiView.h"
#include "CSystemFileDialogCallback.h"
#include <string>
#include <list>
#include <functional>

class CVideoPlayer;
class CVideoYUVShader;
class CVideoAudioChannel;
class CSlrString;

class CGuiViewVideoPlayer : public CGuiView, public CSystemFileDialogCallback
{
public:
    CGuiViewVideoPlayer(const char *name, float posX, float posY, float posZ,
                         float sizeX, float sizeY);
    virtual ~CGuiViewVideoPlayer();

    virtual void RenderImGui() override;

    // Open and play a video file programmatically
    void OpenFile(const char *filePath);

    // Localization support (same pattern as CGuiViewMusicPlaylist)
    static void SetTranslateLabelFunc(const char *(*fn)(const char *key));

    // CSystemFileDialogCallback
    virtual void SystemDialogFileOpenSelected(CSlrString *path) override;
    virtual void SystemDialogFileOpenCancelled() override;
    virtual void SystemDialogFileSaveSelected(CSlrString *path) override {}
    virtual void SystemDialogFileSaveCancelled() override {}

private:
    CVideoPlayer *videoPlayer = nullptr;
    CVideoYUVShader *yuvShader = nullptr;

    // Playback state
    std::string currentFilePath;
    bool isPlaying = false;
    bool wasSeeking = false;
    float seekPosition = 0.0f;     // 0.0 to 1.0 normalized
    float volumeGain = 1.0f;       // 0.0 to 15.0 — amplifies video audio
    bool autoLoad = true;           // re-open last file on startup
    bool didAutoLoad = false;       // guard: only auto-load once per session

    // File dialog
    std::list<CSlrString *> videoExtensions;

    // UI rendering
    void RenderTransportControls();
    void RenderTimeline();
    void RenderVideoFrame(float availW, float availH);
    void CloseVideo();

    // ImDrawList callback data — member (not static) so multiple instances are safe
    struct ShaderCallbackData
    {
        CVideoPlayer *player = nullptr;
        CVideoYUVShader *shader = nullptr;
        float x = 0, y = 0, w = 0, h = 0;
        float screenW = 0, screenH = 0;
    } shaderCbData;

    // Localization
    static const char *(*sTranslateLabelFunc)(const char *key);
    const char *L(const char *key, const char *fallback) const;
};
