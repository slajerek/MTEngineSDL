// CGuiViewVideoPlayer.cpp
#include "CGuiViewVideoPlayer.h"
#include "CVideoPlayer.h"
#include "CVideoYUVShader.h"
#include "CVideoAudioChannel.h"
#include "SND_Main.h"
#include "SYS_FileSystem.h"
#include "CSlrString.h"
#include "CConfigStorageHjson.h"
#include "SYS_DefaultConfig.h"
#include "DBG_Log.h"
#include "IconsFontAwesome_c.h"
#include "imgui.h"
#include <cstdio>
#include <cstring>

const char *(*CGuiViewVideoPlayer::sTranslateLabelFunc)(const char *key) = nullptr;

void CGuiViewVideoPlayer::SetTranslateLabelFunc(const char *(*fn)(const char *key))
{
    sTranslateLabelFunc = fn;
}

const char *CGuiViewVideoPlayer::L(const char *key, const char *fallback) const
{
    if (sTranslateLabelFunc)
    {
        const char *t = sTranslateLabelFunc(key);
        if (t && strcmp(t, key) != 0) return t;
    }
    return fallback;
}

CGuiViewVideoPlayer::CGuiViewVideoPlayer(const char *name, float posX, float posY,
                                           float posZ, float sizeX, float sizeY)
    : CGuiView(name, posX, posY, posZ, sizeX, sizeY)
{
    videoExtensions.push_back(new CSlrString("webm"));
}

CGuiViewVideoPlayer::~CGuiViewVideoPlayer()
{
    CloseVideo();
    if (yuvShader) { delete yuvShader; yuvShader = nullptr; }
    for (auto *ext : videoExtensions) delete ext;
    videoExtensions.clear();
}

void CGuiViewVideoPlayer::CloseVideo()
{
    if (videoPlayer)
    {
        CVideoAudioChannel *audio = videoPlayer->GetAudioChannel();
        if (audio && audio->isActive) SND_RemoveChannel(audio);
        videoPlayer->Close();
        delete videoPlayer;
        videoPlayer = nullptr;
    }
    isPlaying = false;
    seekPosition = 0.0f;
    currentFilePath.clear();
}

void CGuiViewVideoPlayer::OpenFile(const char *filePath)
{
    CloseVideo();

    videoPlayer = new CVideoPlayer();
    if (!videoPlayer->Open(filePath))
    {
        LOGError("CGuiViewVideoPlayer: failed to open '%s'", filePath);
        delete videoPlayer;
        videoPlayer = nullptr;
        return;
    }

    if (!yuvShader)
    {
        yuvShader = new CVideoYUVShader();
        yuvShader->Compile();
    }

    // Register audio with mixer
    CVideoAudioChannel *audio = videoPlayer->GetAudioChannel();
    if (audio) SND_AddChannel(audio);

    currentFilePath = filePath;

    // Persist last opened file
    gApplicationDefaultConfig->SetString("videoPlayerLastFile", filePath);

    // Auto-play on open
    videoPlayer->Play();
    isPlaying = true;
}

// File dialog callbacks
void CGuiViewVideoPlayer::SystemDialogFileOpenSelected(CSlrString *path)
{
    char *cPath = path->GetStdASCII();
    OpenFile(cPath);
    delete[] cPath;
}

void CGuiViewVideoPlayer::SystemDialogFileOpenCancelled()
{
    // Nothing to do
}

void CGuiViewVideoPlayer::RenderImGui()
{
    PreRenderImGui();

    // One-time: load saved settings and auto-open last file
    if (!didAutoLoad)
    {
        didAutoLoad = true;
        gApplicationDefaultConfig->GetBool("videoPlayerAutoLoad", &autoLoad, true);

        float savedGain = 1.0f;
        gApplicationDefaultConfig->GetFloat("videoPlayerVolumeGain", &savedGain, 1.0f);
        volumeGain = savedGain;

        if (autoLoad)
        {
            const char *lastFile = nullptr;
            gApplicationDefaultConfig->GetString("videoPlayerLastFile", &lastFile, nullptr);
            if (lastFile && lastFile[0] != '\0' && SYS_FileExists(lastFile))
            {
                OpenFile(lastFile);
            }
        }
    }

    // Transport controls
    RenderTransportControls();

    ImGui::Separator();

    // Video preview fills available space minus timeline height
    float timelineHeight = 30.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float videoAreaH = avail.y - timelineHeight;

    if (videoAreaH > 10.0f)
    {
        RenderVideoFrame(avail.x, videoAreaH);
    }

    // Timeline at bottom
    RenderTimeline();

    // Spacebar toggle play/pause when this window is focused
    if (videoPlayer && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && ImGui::IsKeyPressed(ImGuiKey_Space, false))
    {
        if (isPlaying)
        {
            videoPlayer->Pause();
            isPlaying = false;
        }
        else
        {
            if (videoPlayer->IsFinished())
                videoPlayer->Seek(0.0);
            videoPlayer->Play();
            isPlaying = true;
        }
    }

    // Apply volume gain to audio channel each frame
    if (videoPlayer)
    {
        CVideoAudioChannel *audio = videoPlayer->GetAudioChannel();
        if (audio) audio->volume = volumeGain;
    }

    // Update video player each frame
    if (videoPlayer && isPlaying)
    {
        float dt = ImGui::GetIO().DeltaTime;
        videoPlayer->Update(dt);

        if (videoPlayer->IsFinished())
        {
            isPlaying = false;
        }
    }

    PostRenderImGui();
}

void CGuiViewVideoPlayer::RenderTransportControls()
{
    // Open button
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " Open##video"))
    {
        CSlrString title("Open Video...");
        SYS_DialogOpenFile(this, &videoExtensions, nullptr, &title);
    }

    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();

    // Play/Pause toggle
    if (videoPlayer)
    {
        if (isPlaying)
        {
            if (ImGui::Button(ICON_FA_PAUSE "##vpause"))
            {
                videoPlayer->Pause();
                isPlaying = false;
            }
        }
        else
        {
            if (ImGui::Button(ICON_FA_PLAY "##vplay"))
            {
                if (videoPlayer->IsFinished())
                {
                    videoPlayer->Seek(0.0);
                }
                videoPlayer->Play();
                isPlaying = true;
            }
        }

        ImGui::SameLine();

        // Stop
        if (ImGui::Button(ICON_FA_STOP "##vstop"))
        {
            videoPlayer->Stop();
            videoPlayer->Seek(0.0);
            isPlaying = false;
            seekPosition = 0.0f;
        }
    }
    else
    {
        // Disabled buttons when no video loaded
        ImGui::BeginDisabled();
        ImGui::Button(ICON_FA_PLAY "##vplay");
        ImGui::SameLine();
        ImGui::Button(ICON_FA_STOP "##vstop");
        ImGui::EndDisabled();
    }

    // Volume gain slider
    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    if (ImGui::SliderFloat("##vvolume", &volumeGain, 0.0f, 15.0f, "Vol %.1f"))
    {
        gApplicationDefaultConfig->SetFloat("videoPlayerVolumeGain", &volumeGain);
    }

    // Autoload checkbox
    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    if (ImGui::Checkbox("Auto##vautoload", &autoLoad))
    {
        gApplicationDefaultConfig->SetBool("videoPlayerAutoLoad", &autoLoad);
    }

    // File name display
    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    if (!currentFilePath.empty())
    {
        // Show just the filename, not full path
        size_t lastSlash = currentFilePath.find_last_of("/\\");
        const char *filename = (lastSlash != std::string::npos)
            ? currentFilePath.c_str() + lastSlash + 1
            : currentFilePath.c_str();
        ImGui::TextUnformatted(filename);
    }
    else
    {
        ImGui::TextDisabled("No video loaded");
    }
}

void CGuiViewVideoPlayer::RenderTimeline()
{
    if (!videoPlayer)
    {
        ImGui::BeginDisabled();
        float dummy = 0.0f;
        ImGui::SliderFloat("##vtimeline", &dummy, 0.0f, 1.0f, "");
        ImGui::EndDisabled();
        return;
    }

    double duration = videoPlayer->GetDuration();
    double currentTime = videoPlayer->GetCurrentTime();

    // Only update slider from player position when user is not dragging
    if (!wasSeeking && duration > 0.0)
        seekPosition = (float)(currentTime / duration);

    // Timeline slider
    ImGui::SetNextItemWidth(-1);  // Full width

    // Format time as MM:SS / MM:SS
    int curMin = (int)(currentTime / 60.0);
    int curSec = (int)(currentTime) % 60;
    int durMin = (int)(duration / 60.0);
    int durSec = (int)(duration) % 60;
    char timeLabel[64];
    snprintf(timeLabel, sizeof(timeLabel), "%d:%02d / %d:%02d", curMin, curSec, durMin, durSec);

    if (ImGui::SliderFloat("##vtimeline", &seekPosition, 0.0f, 1.0f, timeLabel))
    {
        // User is dragging — mark seeking
        wasSeeking = true;
    }

    // Perform seek when user releases the slider
    if (wasSeeking && !ImGui::IsItemActive())
    {
        double seekTime = seekPosition * duration;
        videoPlayer->Seek(seekTime);
        wasSeeking = false;
    }
}

void CGuiViewVideoPlayer::RenderVideoFrame(float availW, float availH)
{
    if (!videoPlayer || !yuvShader)
    {
        // Draw placeholder
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, ImVec2(pos.x + availW, pos.y + availH),
                          IM_COL32(20, 20, 20, 255));
        // Center "No video" text
        const char *text = "Drop or open a .webm file";
        ImVec2 textSize = ImGui::CalcTextSize(text);
        ImVec2 textPos(pos.x + (availW - textSize.x) * 0.5f,
                       pos.y + (availH - textSize.y) * 0.5f);
        dl->AddText(textPos, IM_COL32(100, 100, 100, 255), text);
        ImGui::Dummy(ImVec2(availW, availH));
        return;
    }

    GLuint texY = videoPlayer->GetYTexture();
    if (texY == 0)
    {
        ImGui::Dummy(ImVec2(availW, availH));
        return;
    }

    int vw = videoPlayer->GetVideoWidth();
    int vh = videoPlayer->GetVideoHeight();
    if (vw == 0 || vh == 0) { ImGui::Dummy(ImVec2(availW, availH)); return; }

    // Compute aspect-ratio-preserving rectangle within available area
    float videoAspect = (float)vw / (float)vh;
    float areaAspect = availW / availH;

    float drawW, drawH, offsetX = 0, offsetY = 0;
    if (areaAspect > videoAspect)
    {
        drawH = availH;
        drawW = drawH * videoAspect;
        offsetX = (availW - drawW) * 0.5f;
    }
    else
    {
        drawW = availW;
        drawH = drawW / videoAspect;
        offsetY = (availH - drawH) * 0.5f;
    }

    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    ImDrawList *drawList = ImGui::GetWindowDrawList();

    // Dark background for letterbox/pillarbox
    drawList->AddRectFilled(cursorPos,
        ImVec2(cursorPos.x + availW, cursorPos.y + availH),
        IM_COL32(0, 0, 0, 255));

    // Shader callback data — stored in member struct to survive deferred rendering
    // Coordinates must be relative to the GL viewport origin (= viewport->Pos),
    // not desktop-absolute, because the shader maps them to NDC within the viewport.
    ImGuiViewport *viewport = ImGui::GetMainViewport();
    shaderCbData.player = videoPlayer;
    shaderCbData.shader = yuvShader;
    shaderCbData.x = (cursorPos.x - viewport->Pos.x) + offsetX;
    shaderCbData.y = (cursorPos.y - viewport->Pos.y) + offsetY;
    shaderCbData.w = drawW;
    shaderCbData.h = drawH;
    shaderCbData.screenW = viewport->Size.x;
    shaderCbData.screenH = viewport->Size.y;

    drawList->AddCallback([](const ImDrawList*, const ImDrawCmd* cmd) {
        auto *d = (CGuiViewVideoPlayer::ShaderCallbackData *)cmd->UserCallbackData;
        bool fullRange = (d->player->GetColorRange() == 1);
        d->shader->Render(
            d->player->GetYTexture(), d->player->GetUTexture(),
            d->player->GetVTexture(), d->player->GetATexture(),
            d->player->HasAlpha(), 1.0f,
            d->player->GetColorSpace(), fullRange,
            d->x, d->y, d->w, d->h,
            d->screenW, d->screenH);
    }, &shaderCbData);

    drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);

    ImGui::Dummy(ImVec2(availW, availH));
}
