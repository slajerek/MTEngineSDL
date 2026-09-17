#include "CYoloModelManager.h"

#if MT_ENABLE_YOLO

#include "SYS_DefaultConfig.h"
#include "DBG_Log.h"

#include <filesystem>

namespace fs = std::filesystem;

static const char *kCfgModelsDir  = "yolo.models.dir";
static const char *kCfgSelectedId = "yolo.models.selectedId";
static const char *kCfgPathPrefix = "yolo.models.path.";
static const char *kCfgCustomCount = "yolo.custom.count";
static const char *kCfgCustomPathPrefix = "yolo.custom.path.";

// One id per PATH. std::hash is not stable across runs or platforms, so this
// is a plain FNV-1a written out: the id is persisted in settings and read back
// on the next launch, and an id that changes between launches loses the user's
// selection every time.
static std::string MakeCustomId(const std::string &path)
{
	unsigned long long h = 1469598103934665603ULL;
	for (size_t i = 0; i < path.size(); i++)
	{
		h ^= (unsigned char)path[i];
		h *= 1099511628211ULL;
	}
	char buf[32];
	snprintf(buf, sizeof(buf), "custom_%012llx", h & 0xFFFFFFFFFFFFULL);
	return std::string(buf);
}

CYoloModelManager::CYoloModelManager()
{
	// ── THE SHIPPED CATALOGUE IS PERMISSIVE-ONLY ──────────────────────────
	//
	// This is a licence decision, not a taste one, and it is enforced by
	// CTestYoloDetector rather than left to this comment.
	//
	// Every Ultralytics YOLO -- v5, v8, 11 and YOLO26 -- ships under AGPL-3.0,
	// which obliges the WHOLE application that uses it to be open-sourced.
	// Listing one here would put that obligation on every host that copies this
	// template, silently, at the moment a user clicked Download. So the
	// catalogue offers Apache-2.0 models only.
	//
	// A user who holds an Ultralytics enterprise licence, or whose own
	// application is AGPL, reaches those models through AddCustomModel: the
	// choice is theirs, made deliberately, and the file never came from us.
	defaultModels.push_back({
		"yolox_s",
		"YOLOX-S (COCO, 640)",
		"opencv/object_detection_yolox",
		"object_detection_yolox_2022nov.onnx",
		"Apache-2.0",
		640,
	});
	defaultModels.push_back({
		"yolox_s_int8",
		"YOLOX-S int8 (COCO, 640, smaller download)",
		"opencv/object_detection_yolox",
		"object_detection_yolox_2022nov_int8.onnx",
		"Apache-2.0",
		640,
	});

	LoadCustomModelsFromConfig();
}

void CYoloModelManager::LoadCustomModelsFromConfig()
{
	if (!gApplicationDefaultConfig)
		return;

	int count = 0;
	gApplicationDefaultConfig->GetInt(kCfgCustomCount, &count, 0);
	if (count <= 0 || count > 256)
		return;

	for (int i = 0; i < count; i++)
	{
		char key[64];
		snprintf(key, sizeof(key), "%s%d", kCfgCustomPathPrefix, i);

		std::string path;
		gApplicationDefaultConfig->GetStdString(key, &path, "");
		if (path.empty())
			continue;

		// A file the user has since deleted or moved is DROPPED, not carried
		// as a broken row: the loader would offer it, the detector would fail
		// to load it, and the user would have no way to tell which.
		std::error_code ec;
		if (!fs::exists(path, ec) || ec)
			continue;

		MT_YoloCustomModel model;
		model.path = path;
		model.displayName = fs::path(path).filename().string();
		model.inputSize = 640;
		model.id = MakeCustomId(path);
		customModels.push_back(model);
	}
}

CYoloModelManager::~CYoloModelManager() {}

const std::vector<MT_YoloModelDef> &CYoloModelManager::GetDefaultModels() const
{
	return defaultModels;
}

const std::vector<MT_YoloCustomModel> &CYoloModelManager::GetCustomModels() const
{
	return customModels;
}

std::string CYoloModelManager::GetDownloadUrl(const std::string &modelId) const
{
	for (const MT_YoloModelDef &m : defaultModels)
	{
		if (m.id == modelId)
			return "https://huggingface.co/" + m.hfRepo + "/resolve/main/" + m.hfFilename;
	}
	return "";
}

static std::string JoinPathSimple(const std::string &dir, const std::string &leaf)
{
	if (dir.empty())
		return leaf;
	const char last = dir.back();
	if (last == '/' || last == '\\')
		return dir + leaf;
	return dir + "/" + leaf;
}

std::string CYoloModelManager::GetPlannedDownloadPath(const std::string &modelId) const
{
	const std::string dir = GetModelsDir();
	for (const MT_YoloModelDef &m : defaultModels)
	{
		if (m.id == modelId)
			return JoinPathSimple(dir, m.hfFilename);
	}
	return "";
}

std::string CYoloModelManager::GetModelsDir() const
{
	// Empty when unset, exactly as CLlamaModelManager::GetModelsDir does. The
	// host asks the user for a folder rather than the engine inventing one:
	// a model is tens of megabytes and where it lands is the user's decision.
	std::string dir;
	if (gApplicationDefaultConfig)
		gApplicationDefaultConfig->GetStdString(kCfgModelsDir, &dir, "");
	return dir;
}

void CYoloModelManager::SetModelFolder(const std::string &dir)
{
	if (!gApplicationDefaultConfig)
		return;
	gApplicationDefaultConfig->SetStdString(kCfgModelsDir, dir);
}

std::string CYoloModelManager::GetModelPath(const std::string &modelId) const
{
	for (const MT_YoloCustomModel &c : customModels)
	{
		if (c.id == modelId)
			return c.path;
	}
	std::string path;
	if (gApplicationDefaultConfig)
	{
		const std::string key = std::string(kCfgPathPrefix) + modelId;
		gApplicationDefaultConfig->GetStdString(key.c_str(), &path, "");
	}
	return path;
}

void CYoloModelManager::SetModelPath(const std::string &modelId, const std::string &path)
{
	if (!gApplicationDefaultConfig)
		return;
	const std::string key = std::string(kCfgPathPrefix) + modelId;
	gApplicationDefaultConfig->SetStdString(key.c_str(), path);
}

bool CYoloModelManager::IsModelPathValid(const std::string &modelId) const
{
	const std::string path = GetModelPath(modelId);
	if (path.empty())
		return false;
	std::error_code ec;
	return fs::exists(path, ec) && !ec;
}

int CYoloModelManager::GetInputSize(const std::string &modelId) const
{
	for (const MT_YoloModelDef &m : defaultModels)
		if (m.id == modelId)
			return m.inputSize;
	for (const MT_YoloCustomModel &c : customModels)
		if (c.id == modelId)
			return c.inputSize;
	return 640;
}

std::string CYoloModelManager::GetLicence(const std::string &modelId) const
{
	for (const MT_YoloModelDef &m : defaultModels)
		if (m.id == modelId)
			return m.licence;
	// A browsed file's licence is the user's business and we do not guess it.
	for (const MT_YoloCustomModel &c : customModels)
		if (c.id == modelId)
			return "chosen by you";
	return "";
}

std::string CYoloModelManager::GetDisplayName(const std::string &modelId) const
{
	for (const MT_YoloModelDef &m : defaultModels)
		if (m.id == modelId)
			return m.displayName;
	for (const MT_YoloCustomModel &c : customModels)
		if (c.id == modelId)
			return c.displayName;
	return "";
}

std::string CYoloModelManager::GetSelectedModelId() const
{
	std::string id;
	if (gApplicationDefaultConfig)
		gApplicationDefaultConfig->GetStdString(kCfgSelectedId, &id, "");
	return id;
}

void CYoloModelManager::SetSelectedModelId(const std::string &modelId)
{
	if (!gApplicationDefaultConfig)
		return;
	gApplicationDefaultConfig->SetStdString(kCfgSelectedId, modelId);
}

std::string CYoloModelManager::AddCustomModel(const std::string &path, int inputSize)
{
	if (path.empty())
		return "";

	std::error_code ec;
	if (!fs::exists(path, ec) || ec)
	{
		LOGError("CYoloModelManager::AddCustomModel: no such file '%s'", path.c_str());
		return "";
	}

	for (const MT_YoloCustomModel &c : customModels)
	{
		if (c.path == path)
			return c.id;
	}

	MT_YoloCustomModel model;
	model.path = path;
	model.displayName = fs::path(path).filename().string();
	model.inputSize = inputSize > 0 ? inputSize : 640;

	// THE ID IS UNIQUE ACROSS PATHS, not across basenames. Two directories
	// each holding a yolox.onnx is the normal case, not an exotic one, and an
	// id built from the filename alone gives both rows the same key -- so
	// every lookup returns the first, and selecting the second row silently
	// loads the first file.
	model.id = MakeCustomId(path);
	customModels.push_back(model);

	if (gApplicationDefaultConfig)
	{
		// The COUNT and the paths, so the list survives a restart. Persisting
		// the selection without the row it names left the next launch with a
		// model the detector could load and the loader could not show.
		int count = (int)customModels.size();
		gApplicationDefaultConfig->SetInt(kCfgCustomCount, &count);

		char key[64];
		snprintf(key, sizeof(key), "%s%d", kCfgCustomPathPrefix, (int)customModels.size() - 1);
		gApplicationDefaultConfig->SetStdString(key, path);
	}
	return model.id;
}

#endif // MT_ENABLE_YOLO
