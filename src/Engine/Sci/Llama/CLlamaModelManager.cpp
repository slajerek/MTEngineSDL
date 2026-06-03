#include "CLlamaModelManager.h"

#include "SYS_DefaultConfig.h"
#include "SYS_FileSystem.h"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

// ── Config key constants ───────────────────────────────────────────────────────

static const char *kCfgModelsDir           = "llama.models.dir";
static const char *kCfgAutoSaveChatFolder  = "llama.autoSaveChatFolder";
static const char *kCfgSelectedId          = "llama.models.selectedId";
static const char *kCfgWasLoaded           = "llama.models.wasLoaded";
static const char *kCfgSystemPrompt        = "llama.systemPrompt";
static const char *kCfgPersistSystemPrompt = "llama.persistSystemPrompt";
static const char *kCfgEnableThinking      = "llama.enableThinking";

// Legacy single-path key used by the initial helper.
static const char *kCfgLegacyModelPath     = "llama.modelPath";

// Custom models
static const char *kCfgCustomCount         = "llama.custom.count";

// ── Constructor / Destructor ────────────────────────────────────────────────────

CLlamaModelManager::CLlamaModelManager()
{
	//	std::string id;
	//	std::string displayName;
	//	std::string hfRepo;
	//	std::string hfFilename;

		defaultModels.push_back({
                "gpt-oss-20b",
                "gpt-oss-20b",
                "ggml-org/gpt-oss-20b-GGUF",
                "gpt-oss-20b-mxfp4.gguf",
        });
        defaultModels.push_back({
                "qwen3.5",
                "Qwen3.5-35B-A3B (Unsloth)",
                "unsloth/Qwen3.5-35B-A3B-GGUF",
                "Qwen3.5-35B-A3B-Q4_K_M.gguf",
        });
	// does not work correctly with current llama:
//		defaultModels.push_back({
//				"gemma-4-31b-jang-4m-crack",
//				"Gemma-4-31B-JANG_4M-CRACK",
//				"douyamv/Gemma-4-31B-JANG_4M-CRACK-GGUF",
//				"gemma-4-31b-jang-crack-Q4_K_M.gguf",
//		});

        // One-time migration: if legacy key is set and no selected model yet,
        // assign it as the gpt-oss path (best-effort).
        if (gApplicationDefaultConfig)
        {
                std::string selected;
                gApplicationDefaultConfig->GetStdString(kCfgSelectedId, &selected, "");
                if (selected.empty())
                {
                        std::string legacy;
                        gApplicationDefaultConfig->GetStdString(kCfgLegacyModelPath, &legacy, "");
                        if (!legacy.empty())
                        {
                                SetModelPath("gpt-oss-20b", legacy);
                                SetSelectedModelId("gpt-oss-20b");
                        }
                }
        }

        // Load persisted custom models
        LoadCustomModels();

        // Auto-discover models already present in the configured folder
        ScanModelsFolder();
}

CLlamaModelManager::~CLlamaModelManager() = default;

// ── Default models ──────────────────────────────────────────────────────────────

const std::vector<MT_LlamaModelDef> &CLlamaModelManager::GetDefaultModels() const
{
        return defaultModels;
}

// ── Key helpers ─────────────────────────────────────────────────────────────────

std::string CLlamaModelManager::MakeModelKeySuffix(const std::string &modelId) const
{
        std::string out = modelId;
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                        return (char)c;
                return '_';
        });
        return out;
}

// ── Shared model data ───────────────────────────────────────────────────────────

std::string CLlamaModelManager::GetModelsDir() const
{
        std::string dir;
        if (gApplicationDefaultConfig)
                gApplicationDefaultConfig->GetStdString(kCfgModelsDir, &dir, "");
        return dir;
}

void CLlamaModelManager::SetModelFolder(const std::string &dir)
{
        if (!gApplicationDefaultConfig)
                return;
        std::string d = dir;
        gApplicationDefaultConfig->SetStdString(kCfgModelsDir, &d);
}

std::string CLlamaModelManager::GetModelFolder() const
{
        return GetModelsDir();
}

void CLlamaModelManager::SetAutoSaveChatFolder(const std::string &path)
{
        if (!gApplicationDefaultConfig)
                return;
        std::string p = path;
        gApplicationDefaultConfig->SetStdString(kCfgAutoSaveChatFolder, &p);
}

std::string CLlamaModelManager::GetAutoSaveChatFolder() const
{
        std::string path;
        if (gApplicationDefaultConfig)
                gApplicationDefaultConfig->GetStdString(kCfgAutoSaveChatFolder, &path, "");
        return path;
}

std::string CLlamaModelManager::GetSelectedModelId() const
{
        std::string id;
        if (gApplicationDefaultConfig)
                gApplicationDefaultConfig->GetStdString(kCfgSelectedId, &id, "");
        return id;
}

void CLlamaModelManager::SetSelectedModelId(const std::string &modelId)
{
        if (!gApplicationDefaultConfig)
                return;
        std::string id = modelId;
        gApplicationDefaultConfig->SetStdString(kCfgSelectedId, &id);
}

std::string CLlamaModelManager::GetModelPath(const std::string &modelId) const
{
        // Custom models store path directly in the in-memory struct
        for (const auto &cm : customModels)
        {
                if (cm.id == modelId)
                        return cm.path;
        }

        // Default models store path in config
        std::string path;
        if (!gApplicationDefaultConfig)
                return path;

        std::string key = "llama.models." + MakeModelKeySuffix(modelId) + ".path";
        gApplicationDefaultConfig->GetStdString(key.c_str(), &path, "");
        return path;
}

void CLlamaModelManager::SetModelPath(const std::string &modelId, const std::string &path)
{
        // Custom models: update in-memory + re-persist
        for (auto &cm : customModels)
        {
                if (cm.id == modelId)
                {
                        cm.path = path;
                        SaveCustomModels();
                        return;
                }
        }

        // Default models: store in config
        if (!gApplicationDefaultConfig)
                return;

        std::string key = "llama.models." + MakeModelKeySuffix(modelId) + ".path";
        std::string p = path;
        gApplicationDefaultConfig->SetStdString(key.c_str(), &p);
}

bool CLlamaModelManager::IsModelPathValid(const std::string &modelId) const
{
        std::string path = GetModelPath(modelId);
        if (path.empty())
                return false;
        return SYS_FileExists(path.c_str());
}

std::string CLlamaModelManager::GetModelFileNameFromPath(const std::string &modelId) const
{
        std::string path = GetModelPath(modelId);
        if (path.empty())
                return "";

        char *tmp = SYS_GetFileName(path.c_str());
        if (!tmp)
                return "";

        std::string out = tmp;
        free(tmp);
        return out;
}

std::string CLlamaModelManager::GetDownloadUrl(const std::string &modelId) const
{
        for (const auto &m : defaultModels)
        {
                if (m.id == modelId)
                {
                        return "https://huggingface.co/" + m.hfRepo + "/resolve/main/" + m.hfFilename;
                }
        }
        return "";
}

static std::string JoinPathSimple(const std::string &dir, const std::string &leaf)
{
        if (dir.empty())
                return leaf;
        char last = dir.back();
        if (last == '/' || last == '\\')
                return dir + leaf;
        return dir + "/" + leaf;
}

std::string CLlamaModelManager::GetPlannedDownloadPath(const std::string &modelId) const
{
        std::string dir = GetModelsDir();
        if (dir.empty())
                return "";

        for (const auto &m : defaultModels)
        {
                if (m.id == modelId)
                        return JoinPathSimple(JoinPathSimple(dir, m.hfRepo), m.hfFilename);
        }

        return "";
}

bool CLlamaModelManager::GetWasModelLoaded() const
{
        if (!gApplicationDefaultConfig)
                return false;
        return gApplicationDefaultConfig->GetBool(kCfgWasLoaded, false);
}

void CLlamaModelManager::SetWasModelLoaded(bool loaded)
{
        if (!gApplicationDefaultConfig)
                return;
        gApplicationDefaultConfig->SetBool(kCfgWasLoaded, &loaded);
}

std::string CLlamaModelManager::GetSystemPrompt() const
{
        std::string prompt;
        if (gApplicationDefaultConfig)
                gApplicationDefaultConfig->GetStdString(kCfgSystemPrompt, &prompt, "");
        return prompt;
}

void CLlamaModelManager::SetSystemPrompt(const std::string &prompt)
{
        if (!gApplicationDefaultConfig)
                return;
        std::string p = prompt;
        gApplicationDefaultConfig->SetStdString(kCfgSystemPrompt, &p);
}

bool CLlamaModelManager::GetPersistSystemPrompt() const
{
        if (!gApplicationDefaultConfig)
                return true;
        return gApplicationDefaultConfig->GetBool(kCfgPersistSystemPrompt, true);
}

void CLlamaModelManager::SetPersistSystemPrompt(bool persist)
{
        if (!gApplicationDefaultConfig)
                return;
        gApplicationDefaultConfig->SetBool(kCfgPersistSystemPrompt, &persist);
}

bool CLlamaModelManager::GetEnableThinking() const
{
        if (!gApplicationDefaultConfig)
                return true;
        return gApplicationDefaultConfig->GetBool(kCfgEnableThinking, true);
}

void CLlamaModelManager::SetEnableThinking(bool enable)
{
        if (!gApplicationDefaultConfig)
                return;
        gApplicationDefaultConfig->SetBool(kCfgEnableThinking, &enable);
}

// ── Global load + generation params ────────────────────────────────────────────

void CLlamaModelManager::SaveGlobalParams(const MT_LlamaLoadParams    &lp,
                                          const MT_LlamaGenerateParams &gp)
{
        if (!gApplicationDefaultConfig)
                return;

        int nCtx       = lp.n_ctx;
        int nThreads   = lp.n_threads;
        int nGpuLayers = lp.n_gpu_layers;
        gApplicationDefaultConfig->SetInt("llama.global.n_ctx",        &nCtx);
        gApplicationDefaultConfig->SetInt("llama.global.n_threads",    &nThreads);
        gApplicationDefaultConfig->SetInt("llama.global.n_gpu_layers", &nGpuLayers);

        int   maxTok = gp.max_tokens;
        float temp   = gp.temperature;
        int   topK   = gp.top_k;
        float topP   = gp.top_p;
        float minP   = gp.min_p;
        int   seed   = (int)gp.seed;
        gApplicationDefaultConfig->SetInt  ("llama.global.max_tokens",  &maxTok);
        gApplicationDefaultConfig->SetFloat("llama.global.temperature", &temp);
        gApplicationDefaultConfig->SetInt  ("llama.global.top_k",       &topK);
        gApplicationDefaultConfig->SetFloat("llama.global.top_p",       &topP);
        gApplicationDefaultConfig->SetFloat("llama.global.min_p",       &minP);
        gApplicationDefaultConfig->SetInt  ("llama.global.seed",        &seed);
        float repPenalty = gp.repeat_penalty;
        int   repLastN   = gp.repeat_last_n;
        gApplicationDefaultConfig->SetFloat("llama.global.repeat_penalty", &repPenalty);
        gApplicationDefaultConfig->SetInt  ("llama.global.repeat_last_n",  &repLastN);
        float dryMul = gp.dry_multiplier;
        float dryBase = gp.dry_base;
        int   dryAllowed = gp.dry_allowed_length;
        int   dryLastN = gp.dry_penalty_last_n;
        gApplicationDefaultConfig->SetFloat("llama.global.dry_multiplier",     &dryMul);
        gApplicationDefaultConfig->SetFloat("llama.global.dry_base",           &dryBase);
        gApplicationDefaultConfig->SetInt  ("llama.global.dry_allowed_length", &dryAllowed);
        gApplicationDefaultConfig->SetInt  ("llama.global.dry_penalty_last_n", &dryLastN);
}

void CLlamaModelManager::LoadGlobalParams(MT_LlamaLoadParams    &lpOut,
                                          MT_LlamaGenerateParams &gpOut) const
{
        if (!gApplicationDefaultConfig)
                return;

        gApplicationDefaultConfig->GetInt("llama.global.n_ctx",        &lpOut.n_ctx,        lpOut.n_ctx);
        gApplicationDefaultConfig->GetInt("llama.global.n_threads",    &lpOut.n_threads,    lpOut.n_threads);
        gApplicationDefaultConfig->GetInt("llama.global.n_gpu_layers", &lpOut.n_gpu_layers, lpOut.n_gpu_layers);

        gApplicationDefaultConfig->GetInt  ("llama.global.max_tokens",  &gpOut.max_tokens,  gpOut.max_tokens);
        gApplicationDefaultConfig->GetFloat("llama.global.temperature", &gpOut.temperature, gpOut.temperature);
        gApplicationDefaultConfig->GetInt  ("llama.global.top_k",       &gpOut.top_k,       gpOut.top_k);
        gApplicationDefaultConfig->GetFloat("llama.global.top_p",       &gpOut.top_p,       gpOut.top_p);
        gApplicationDefaultConfig->GetFloat("llama.global.min_p",       &gpOut.min_p,       gpOut.min_p);
        int seed = (int)gpOut.seed;
        gApplicationDefaultConfig->GetInt("llama.global.seed", &seed, seed);
        gpOut.seed = (uint32_t)seed;
        gApplicationDefaultConfig->GetFloat("llama.global.repeat_penalty", &gpOut.repeat_penalty, gpOut.repeat_penalty);
        gApplicationDefaultConfig->GetInt  ("llama.global.repeat_last_n",  &gpOut.repeat_last_n,  gpOut.repeat_last_n);
        gApplicationDefaultConfig->GetFloat("llama.global.dry_multiplier",     &gpOut.dry_multiplier,     gpOut.dry_multiplier);
        gApplicationDefaultConfig->GetFloat("llama.global.dry_base",           &gpOut.dry_base,           gpOut.dry_base);
        gApplicationDefaultConfig->GetInt  ("llama.global.dry_allowed_length", &gpOut.dry_allowed_length, gpOut.dry_allowed_length);
        gApplicationDefaultConfig->GetInt  ("llama.global.dry_penalty_last_n", &gpOut.dry_penalty_last_n, gpOut.dry_penalty_last_n);
}
// ── Custom models ───────────────────────────────────────────────────────────────

const std::vector<MT_LlamaCustomModel> &CLlamaModelManager::GetCustomModels() const
{
        return customModels;
}

bool CLlamaModelManager::IsCustomModelId(const std::string &id) const
{
        for (const auto &cm : customModels)
                if (cm.id == id)
                        return true;
        return false;
}

void CLlamaModelManager::AddCustomModel(const MT_LlamaCustomModel &m)
{
        // Replace if already exists (same id)
        for (auto &existing : customModels)
        {
                if (existing.id == m.id)
                {
                        existing = m;
                        SaveCustomModels();
                        return;
                }
        }
        customModels.push_back(m);
        SaveCustomModels();
}

void CLlamaModelManager::RemoveCustomModel(const std::string &id)
{
        customModels.erase(
                std::remove_if(customModels.begin(), customModels.end(),
                               [&id](const MT_LlamaCustomModel &m){ return m.id == id; }),
                customModels.end());
        SaveCustomModels();
}

void CLlamaModelManager::SetCustomModelParams(const std::string &id,
                                              const MT_LlamaLoadParams    &lp,
                                              const MT_LlamaGenerateParams &gp)
{
        for (auto &cm : customModels)
        {
                if (cm.id == id)
                {
                        cm.loadParams = lp;
                        cm.defaultGenParams  = gp;
                        SaveCustomModels();
                        return;
                }
        }
}

bool CLlamaModelManager::GetCustomModelParams(const std::string &id,
                                              MT_LlamaLoadParams    &lpOut,
                                              MT_LlamaGenerateParams &gpOut) const
{
        for (const auto &cm : customModels)
        {
                if (cm.id == id)
                {
                        lpOut = cm.loadParams;
                        gpOut = cm.defaultGenParams;
                        return true;
                }
        }
        return false;
}

void CLlamaModelManager::SaveCustomModels()
{
        if (!gApplicationDefaultConfig)
                return;

        int count = (int)customModels.size();
        gApplicationDefaultConfig->SetInt(kCfgCustomCount, &count);

        for (int i = 0; i < count; ++i)
        {
                const auto &cm = customModels[i];
                char prefix[64];
                snprintf(prefix, sizeof(prefix), "llama.custom.%d", i);

                std::string keyId   = std::string(prefix) + ".id";
                std::string keyName = std::string(prefix) + ".displayName";
                std::string keyPath = std::string(prefix) + ".path";

                gApplicationDefaultConfig->SetStdString(keyId.c_str(),   cm.id);
                gApplicationDefaultConfig->SetStdString(keyName.c_str(), cm.displayName);
                gApplicationDefaultConfig->SetStdString(keyPath.c_str(), cm.path);

                // load params
                std::string keyCtx     = std::string(prefix) + ".n_ctx";
                std::string keyThreads = std::string(prefix) + ".n_threads";
                std::string keyGpu     = std::string(prefix) + ".n_gpu_layers";
                int nCtx        = cm.loadParams.n_ctx;
                int nThreads    = cm.loadParams.n_threads;
                int nGpuLayers  = cm.loadParams.n_gpu_layers;
                gApplicationDefaultConfig->SetInt(keyCtx.c_str(),     &nCtx);
                gApplicationDefaultConfig->SetInt(keyThreads.c_str(), &nThreads);
                gApplicationDefaultConfig->SetInt(keyGpu.c_str(),     &nGpuLayers);

                // gen params
                std::string keyMaxTok = std::string(prefix) + ".max_tokens";
                std::string keyTemp   = std::string(prefix) + ".temperature";
                std::string keySeed   = std::string(prefix) + ".seed";
                int   maxTok  = cm.defaultGenParams.max_tokens;
                float temp    = cm.defaultGenParams.temperature;
                int   seed    = (int)cm.defaultGenParams.seed; // store as int; restores correctly
                gApplicationDefaultConfig->SetInt(keyMaxTok.c_str(), &maxTok);
                gApplicationDefaultConfig->SetFloat(keyTemp.c_str(), &temp);
                gApplicationDefaultConfig->SetInt(keySeed.c_str(),   &seed);
        }
}

void CLlamaModelManager::LoadCustomModels()
{
        if (!gApplicationDefaultConfig)
                return;

        int count = 0;
        gApplicationDefaultConfig->GetInt(kCfgCustomCount, &count, 0);

        customModels.clear();
        for (int i = 0; i < count; ++i)
        {
                char prefix[64];
                snprintf(prefix, sizeof(prefix), "llama.custom.%d", i);

                std::string keyId   = std::string(prefix) + ".id";
                std::string keyName = std::string(prefix) + ".displayName";
                std::string keyPath = std::string(prefix) + ".path";

                MT_LlamaCustomModel cm;
                gApplicationDefaultConfig->GetStdString(keyId.c_str(),   &cm.id,          "");
                gApplicationDefaultConfig->GetStdString(keyName.c_str(), &cm.displayName, "");
                gApplicationDefaultConfig->GetStdString(keyPath.c_str(), &cm.path,        "");

                if (cm.id.empty())
                        continue;

                // load params
                std::string keyCtx     = std::string(prefix) + ".n_ctx";
                std::string keyThreads = std::string(prefix) + ".n_threads";
                std::string keyGpu     = std::string(prefix) + ".n_gpu_layers";
                gApplicationDefaultConfig->GetInt(keyCtx.c_str(),     &cm.loadParams.n_ctx,        8192);
                gApplicationDefaultConfig->GetInt(keyThreads.c_str(), &cm.loadParams.n_threads,    0);
                gApplicationDefaultConfig->GetInt(keyGpu.c_str(),     &cm.loadParams.n_gpu_layers, -1);

                // gen params
                std::string keyMaxTok = std::string(prefix) + ".max_tokens";
                std::string keyTemp   = std::string(prefix) + ".temperature";
                std::string keySeed   = std::string(prefix) + ".seed";
                gApplicationDefaultConfig->GetInt(keyMaxTok.c_str(), &cm.defaultGenParams.max_tokens,  1024);
                gApplicationDefaultConfig->GetFloat(keyTemp.c_str(), &cm.defaultGenParams.temperature, 0.8f);
                int seed = (int)0xFFFFFFFFu;
                gApplicationDefaultConfig->GetInt(keySeed.c_str(), &seed, (int)0xFFFFFFFFu);
                cm.defaultGenParams.seed = (uint32_t)seed;

                customModels.push_back(cm);
        }
}

// ── Folder scan ─────────────────────────────────────────────────────────────────

int CLlamaModelManager::ScanModelsFolder()
{
        std::string dir = GetModelsDir();
        if (dir.empty())
                return 0;

        fs::path rootPath;
        try { rootPath = fs::canonical(dir); }
        catch (...) { rootPath = fs::path(dir); }

        if (!fs::is_directory(rootPath))
                return 0;

        int found = 0;

        try
        {
                for (const auto &entry : fs::recursive_directory_iterator(rootPath, fs::directory_options::skip_permission_denied))
                {
                        if (!entry.is_regular_file())
                                continue;

                        fs::path filePath = entry.path();
                        if (filePath.extension() != ".gguf")
                                continue;

                        // Relative path from root, using forward slashes
                        std::string relStr = fs::relative(filePath, rootPath).generic_string();

                        // Check if this matches a default model by hfRepo/hfFilename
                        bool matchedDefault = false;
                        for (const auto &m : defaultModels)
                        {
                                std::string expected = m.hfRepo + "/" + m.hfFilename;
                                if (relStr == expected)
                                {
                                        SetModelPath(m.id, filePath.string());
                                        matchedDefault = true;
                                        ++found;
                                        break;
                                }
                        }

                        if (matchedDefault)
                                continue;

                        // Check if already tracked as a custom model
                        bool alreadyTracked = false;
                        for (const auto &cm : customModels)
                        {
                                if (cm.path == filePath.string())
                                {
                                        alreadyTracked = true;
                                        break;
                                }
                        }

                        if (!alreadyTracked)
                        {
                                // Generate a stable id from the relative path
                                std::string idStr = "scan_" + relStr;
                                std::transform(idStr.begin(), idStr.end(), idStr.begin(), [](unsigned char c) {
                                        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                                                return (char)c;
                                        return '_';
                                });

                                MT_LlamaCustomModel cm;
                                cm.id          = idStr;
                                cm.displayName = filePath.filename().string();
                                cm.path        = filePath.string();
                                AddCustomModel(cm);
                                ++found;
                        }
                }
        }
        catch (...) {}

        return found;
}
